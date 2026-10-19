#include "patch/static_call.hpp"

#include "memory/vmm/vma/vma.hpp"

extern "C" {
[[gnu::visibility("hidden")]] extern const std::uint8_t __text_start[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __text_end[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __start_static_calls[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __stop_static_calls[];
}

namespace kernel::patch {
auto StaticCallPatcher::apply_patches() noexcept -> std::expected<void, Error> {
  using namespace memory::vmm;
  const std::size_t table_bytes = __stop_static_calls - __start_static_calls;
  const std::size_t count = table_bytes / sizeof(StaticCallEntry);
  if (count == 0) return {};

  const std::span entries{
      reinterpret_cast<const StaticCallEntry*>(__start_static_calls),
      count,
  };

  // Obtain .text boundaries from the linker script
  const auto text_start = memory::VirtualAddress{__text_start};
  const auto text_end = memory::VirtualAddress{__text_end}.align_up(memory::PAGE_SIZE);
  const std::size_t text_size = text_end - text_start;

  // Create a single RW (non-executatble) alias for the .text section
  AddressSpace* kspace = kernel_space();
  auto alias_res =
      kspace->create_alias(text_start, text_size, AccessFlags::Read | AccessFlags::Write);
  if (!alias_res) return std::unexpected(alias_res.error());
  const memory::VirtualAddress alias_base = *alias_res;

  ZydisDecoder decoder;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisDecoderEnableMode(&decoder, ZYDIS_DECODER_MODE_MINIMAL, ZYAN_TRUE);

  const hw::CpuInfo* info = hw::profile_manager.get_current();
  std::expected<void, Error> status{};
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const memory::VirtualAddress curr_site = entries[i].site_address();

    // Skip if we already processed this call site earlier in the scan
    bool already_processed = false;
    for (std::size_t prev = 0; prev < i; ++prev) {
      if (entries[prev].site_address() == curr_site) {
        already_processed = true;
        break;
      }
    }

    if (already_processed) continue;

    // Scan all entried targeting `curr_site` and pick the highest priority
    // whose feature requirements are satisfied.
    const StaticCallEntry* best_entry = nullptr;
    for (std::size_t j = i; j < entries.size(); ++j) {
      if (entries[j].site_address() != curr_site) continue;
      if (!entries[j].is_supported(info)) continue;

      if (best_entry == nullptr || entries[j].priority >= best_entry->priority)
        best_entry = &entries[j];
    }

    if (best_entry) {
      status = patch_site(decoder, *best_entry, text_start, text_size, alias_base);
      if (!status) break;
    }
  }

  auto _ = kspace->destroy_alias(alias_base);
  serialize_pipeline(info);
  return status;
}

auto StaticCallPatcher::patch_site(const ZydisDecoder& decoder, const StaticCallEntry& entry,
                                   memory::VirtualAddress text_start, std::size_t text_size,
                                   memory::VirtualAddress alias_base) noexcept
    -> std::expected<void, Error> {
  const memory::VirtualAddress site_va = entry.site_address();
  const memory::VirtualAddress target_va = entry.target_address();

  if (site_va < text_start || site_va + kDirectBranchSize > text_start + text_size)
    return std::unexpected(Error::SiteOutOfTextBounds);

  const std::size_t max_readable =
      std::min<std::size_t>(kMaxSiteSize, (text_start + text_size) - site_va);

  // Translate the .text address to the corresponding alias address
  const std::size_t text_offset = site_va - text_start;
  auto* const writable_site = memory::VirtualAddress{alias_base + text_offset}.as<std::uint8_t>();
  const auto* const rx_site = site_va.as<const std::uint8_t>();

  // Decode the existing instruction at the patch site
  ZydisDecodedInstruction existing_insn{};
  if (ZYAN_FAILED(
          ZydisDecoderDecodeInstruction(&decoder, nullptr, rx_site, max_readable, &existing_insn)))
    return std::unexpected(Error::ZydisDecodeFailed);

  // Covers either 5 bytes (direct branch) or 6 bytes(FF 25 indirect jmp)
  const std::size_t span_to_cover = std::max<std::size_t>(kDirectBranchSize, existing_insn.length);

  // Case A: Disabling a site (target_va == nullptr)
  if (target_va.is_null()) {
    if (existing_insn.mnemonic == ZYDIS_MNEMONIC_CALL) {
      // In-place CALL -> 5-byte NOP
      if (ZYAN_FAILED(ZydisEncoderNopFill(writable_site, span_to_cover)))
        return std::unexpected(Error::ZydisEncodeFailed);
    } else if (existing_insn.mnemonic == ZYDIS_MNEMONIC_JMP ||
               existing_insn.mnemonic == ZYDIS_MNEMONIC_RET) {
      // Trampoline JMP -> RET (0xC3) + 4-byte NOP
      writable_site[0] = 0xC3;
      if (ZYAN_FAILED(ZydisEncoderNopFill(writable_site + 1, span_to_cover - 1)))
        return std::unexpected(Error::ZydisEncodeFailed);
    } else {
      return std::unexpected(Error::UnexpectedSiteMnemonic);
    }

    return {};
  }

  // Case B: Redirecting a site to target_va via 5-byte direct CALL/JMP rel32
  ZydisMnemonic target_mnemoic;
  if (existing_insn.mnemonic == ZYDIS_MNEMONIC_CALL ||
      existing_insn.mnemonic == ZYDIS_MNEMONIC_NOP) {
    target_mnemoic = ZYDIS_MNEMONIC_CALL;
  } else if (existing_insn.mnemonic == ZYDIS_MNEMONIC_JMP ||
             existing_insn.mnemonic == ZYDIS_MNEMONIC_RET) {
    target_mnemoic = ZYDIS_MNEMONIC_JMP;
  } else {
    return std::unexpected(Error::UnexpectedSiteMnemonic);
  }

  // Construct the Zydis Encoder request forcing a 5-byte 32-bit near branch
  ZydisEncoderRequest req{};
  req.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
  req.mnemonic = target_mnemoic;
  req.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
  req.branch_width = ZYDIS_BRANCH_WIDTH_32;
  req.operand_count = 1;
  req.operands[0].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
  req.operands[0].imm.u = target_va.value();

  ZyanUSize encoded_len = span_to_cover;
  if (ZYAN_FAILED(ZydisEncoderEncodeInstructionAbsolute(&req, writable_site, &encoded_len,
                                                        site_va.value())))
    return std::unexpected(Error::ZydisEncodeFailed);

  // If replacing a 6-byte initial `FF 25` indirect jump with a 5-byte `E9`
  // direct jump, pad the remaining 6th byte with a 1-byte NOP.
  if (encoded_len < span_to_cover)
    if (ZYAN_FAILED(ZydisEncoderNopFill(writable_site + encoded_len, span_to_cover - encoded_len)))
      return std::unexpected(Error::ZydisEncodeFailed);

  return {};
}

void StaticCallPatcher::serialize_pipeline(const hw::CpuInfo* info) noexcept {
  if (info->has(hw::Feature::SERIALIZE)) {
    asm volatile("serialize" : /* No Output */ : /* No Input */ : "memory");
  } else {
    int a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(0) : "memory");
  }
}
}  // namespace kernel::patch