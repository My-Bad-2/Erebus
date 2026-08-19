#include "hal/patcher.hpp"

#include "utils/logger.hpp"

#include <string.h>

#include <Zydis/Zydis.h>

namespace kernel::hw::patcher {
extern "C" const AltInstr __start_altinstructions[];
extern "C" const AltInstr __stop_altinstructions[];
extern "C" const StaticKeyEntry __start_static_keys[];
extern "C" const StaticKeyEntry __stop_static_keys[];
extern "C" const void *__start_extable;
extern "C" const void *__stop_extable;

namespace {
class WpDisable {
public:
  WpDisable() noexcept : m_cr0(0) {
    asm volatile("mov %%cr0, %0" : "=r"(m_cr0));
    asm volatile("mov %0, %%cr0" ::"r"(m_cr0 & ~(1UL << 16)) : "memory"); // Clear WP
  }

  ~WpDisable() noexcept {
    asm volatile("mov %0, %%cr0" ::"r"(m_cr0) : "memory");
    // Serialize the execution queue
    asm volatile("cpuid" ::"a"(0) : "ebx", "ecx", "edx", "memory");
  }

private:
  uint64_t m_cr0;
};
} // namespace

bool InstructionDecoder::verify_boundaries(const std::uint8_t *code, std::uint8_t expected_len) noexcept {
  if (code[0] == 0xE8 || code[0] == 0xE9) {
    return expected_len == 5; // CALL/JMP rel32
  }

  ZydisDecoder decoder;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

  std::size_t processed = 0;
  ZydisDecodedInstruction insn;

  // Decode until we hit exactly the expected length
  while (processed < expected_len) {
    if (!ZYAN_SUCCESS(
            ZydisDecoderDecodeInstruction(&decoder, nullptr, code + processed, expected_len - processed, &insn))) {
      return false;
    }

    processed += insn.length;
  }

  return processed == expected_len;
}

bool InstructionDecoder::relocate_rip_instructions(std::span<std::uint8_t> payload, std::uintptr_t old_rip,
                                                   std::uintptr_t new_rip) noexcept {
  ZydisDecoder decoder;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

  constexpr std::int64_t MAX_REL32 = std::numeric_limits<std::int32_t>::max();
  constexpr std::int64_t MIN_REL32 = std::numeric_limits<std::int32_t>::min();

  std::size_t offset = 0;
  while (offset < payload.size()) {
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    if (!ZYAN_SUCCESS(
            ZydisDecoderDecodeFull(&decoder, payload.data() + offset, payload.size() - offset, &insn, operands))) {
      break;
    }

    for (int i = 0; i < insn.operand_count_visible; ++i) {
      // Fix Memory [RIP + disp]
      if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[i].mem.base == ZYDIS_REGISTER_RIP) {
        const std::uint8_t disp_offset = insn.raw.disp.offset;
        std::int32_t current_disp;

        klib::memcpy(&current_disp, payload.data() + offset + disp_offset, sizeof(current_disp));

        const std::uintptr_t old_anchor = old_rip + offset + insn.length;
        const std::uintptr_t new_anchor = new_rip + offset + insn.length;
        const std::uintptr_t absolute_target = old_anchor + static_cast<std::int64_t>(current_disp);

        const std::int64_t diff = static_cast<std::int64_t>(absolute_target) - static_cast<std::int64_t>(new_anchor);
        if (diff < MIN_REL32 || diff > MAX_REL32) {
          utils::logger::fatal("Zydis RIP relocation out of 32-bit bounds!\n");
          return false;
        }

        const std::int32_t new_disp = static_cast<std::int32_t>(absolute_target - new_anchor);
        klib::memcpy(payload.data() + offset + disp_offset, &new_disp, sizeof(new_disp));
        break;
      }

      // Fix Branch rel32 / rel8
      if (insn.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE && operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
          operands[i].imm.is_relative) {

        const std::uint8_t imm_offset = insn.raw.imm[0].offset;
        const std::uintptr_t old_anchor = old_rip + offset + insn.length;
        const std::uintptr_t new_anchor = new_rip + offset + insn.length;

        if (insn.raw.imm[0].size == 32) {
          std::int32_t current_disp;
          klib::memcpy(&current_disp, payload.data() + offset + imm_offset, sizeof(current_disp));

          const std::uintptr_t absolute_target = old_anchor + static_cast<std::int64_t>(current_disp);

          const std::int64_t diff = static_cast<std::int64_t>(absolute_target) - static_cast<std::int64_t>(new_anchor);
          if (diff < MIN_REL32 || diff > MAX_REL32) {
            utils::logger::fatal("Zydis RIP relocation out of 32-bit bounds!\n");
            return false;
          }

          const std::int32_t new_disp = static_cast<std::int32_t>(diff);
          klib::memcpy(payload.data() + offset + imm_offset, &new_disp, sizeof(new_disp));
        } else if (insn.raw.imm[0].size == 8) {
          std::int8_t current_disp;
          klib::memcpy(&current_disp, payload.data() + offset + imm_offset, sizeof(current_disp));

          const std::uintptr_t absolute_target = old_anchor + static_cast<std::int64_t>(current_disp);

          const std::int64_t diff = static_cast<std::int64_t>(absolute_target) - static_cast<std::int64_t>(new_anchor);
          if (diff < -128 || diff > 127) {
            utils::logger::fatal("Zydis rel8 relocation out of bounds!\n");
            return false;
          }

          const std::int8_t new_disp = static_cast<std::int8_t>(diff);
          klib::memcpy(payload.data() + offset + imm_offset, &new_disp, sizeof(new_disp));
        }
        break;
      }
    }

    offset += insn.length;
  }

  return true;
}

void InstructionDecoder::pad_with_nops(std::span<std::uint8_t> buffer, const std::uintptr_t target_addr) noexcept {
  if (buffer.empty()) {
    return;
  }

  constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
  const std::size_t dist_to_boundary = CACHE_LINE - (target_addr % CACHE_LINE);

  // Never allow a multi-byte NOP to straddle a 64-byte boundary
  if (dist_to_boundary > 0 && dist_to_boundary < buffer.size()) {
    ZydisEncoderNopFill(buffer.data(), dist_to_boundary);
    ZydisEncoderNopFill(buffer.data() + dist_to_boundary, buffer.size() - dist_to_boundary);
  } else {
    ZydisEncoderNopFill(buffer.data(), buffer.size());
  }
}

void ExceptionTable::sync_trampoline(const std::uintptr_t old_rip, const std::uintptr_t new_rip) noexcept {
  auto *start = static_cast<const ExceptionTableEntry *>(__start_extable);
  auto *stop = static_cast<const ExceptionTableEntry *>(__stop_extable);

  for (auto *entry = start; entry < stop; ++entry) {
    const auto relative_offset = static_cast<std::int64_t>(entry->insn_offset);
    const auto fault_addr = reinterpret_cast<std::uintptr_t>(&entry->insn_offset) + relative_offset;

    if (fault_addr == old_rip) {
      auto *mutable_entry = const_cast<ExceptionTableEntry *>(entry);
      mutable_entry->insn_offset =
          static_cast<std::int32_t>(new_rip - reinterpret_cast<std::uintptr_t>(&entry->insn_offset));
      return;
    }
  }
}

bool BootPatcher::validate_ibt(std::span<const std::uint8_t> payload, const std::uint8_t *target) noexcept {
  // ENDBR64 Opcode: 0xF3 0x0F 0x1E 0xFA
  const bool orig_has_endbr = (target[0] == 0xF3 && target[1] == 0x0F && target[2] == 0x1E && target[3] == 0xFA);

  if (orig_has_endbr) {
    if (payload.size() < 4) {
      return false;
    }

    const bool repl_has_endbr = (payload[0] == 0xF3 && payload[1] == 0x0F && payload[2] == 0x1E && payload[3] == 0xFA);
    // If the original instruction was a valid indirect branch target, the replacement must also start with ENDBR64
    return repl_has_endbr;
  }

  return true;
}

PatchResult BootPatcher::apply_patch(const AltInstr &alt) noexcept {
  const auto target = std::bit_cast<std::uintptr_t>(&alt) + alt.instr_offset;
  auto *dest = reinterpret_cast<std::uint8_t *>(target);

  if (alt.flags == PatchFlags::STRIP_PREFIX) {
    if (dest[0] >= 0xF0 && dest[0] <= 0xF3) {
      dest[0] = 0x90; // NOP
    }

    return {};
  }

  const auto repl = reinterpret_cast<std::uintptr_t>(&alt.repl_offset) + static_cast<std::intptr_t>(alt.repl_offset);

  if (!InstructionDecoder::verify_boundaries(dest, alt.instr_len)) {
    return std::unexpected(PatchError::InstructionBoundaryMismatch);
  }

  if (alt.flags == PatchFlags::OUT_OF_TRAMPOLINE) {
    const auto displacement = static_cast<std::int64_t>(repl) - static_cast<std::int64_t>(target + 2);

    // If the trampoline is within 127 bytes, use a 2-byte rel8 jump
    if (displacement >= -128 && displacement <= 127 && alt.instr_len >= 2) {
      dest[0] = 0xEB; // JMP rel8
      dest[1] = static_cast<std::int8_t>(displacement);

      if (alt.instr_len > 2) {
        InstructionDecoder::pad_with_nops(std::span{dest + 2, static_cast<std::size_t>(alt.instr_len - 2)}, target + 2);
      }
    } else {
      if (alt.instr_len < 5) {
        return std::unexpected(PatchError::TargetTooSmallForTrampoline);
      }

      const auto disp64 = static_cast<std::int64_t>(repl) - static_cast<std::int64_t>(target + 5);

      constexpr std::int64_t MAX_REL32 = std::numeric_limits<std::int32_t>::max();
      constexpr std::int64_t MIN_REL32 = std::numeric_limits<std::int32_t>::min();

      if (disp64 < MIN_REL32 || disp64 > MAX_REL32) {
        utils::logger::fatal("Trampoline displacement out of 32-bit bounds!\n");
        return std::unexpected(PatchError::TargetTooSmallForTrampoline);
      }

      dest[0] = 0xE9; // JMP rel32
      const auto disp = static_cast<std::int32_t>(disp64);
      klib::memcpy(dest + 1, &disp, sizeof(disp));

      if (alt.instr_len > 5) {
        InstructionDecoder::pad_with_nops(std::span{dest + 5, static_cast<std::size_t>(alt.instr_len - 5)}, target + 5);
      }
    }

    ExceptionTable::sync_trampoline(target, repl);
    return {};
  }

  // Inline patching
  std::array<std::uint8_t, 64> payload{};
  if (alt.repl_len > 0) {
    klib::memcpy(payload.data(), reinterpret_cast<const void *>(repl), alt.repl_len);

    if (!validate_ibt(std::span<const std::uint8_t>{payload.data(), alt.repl_len}, dest)) {
      return std::unexpected(PatchError::IBTViolation);
    }

    if (!InstructionDecoder::relocate_rip_instructions(std::span(payload.data(), alt.repl_len), repl, target)) {
      return std::unexpected(PatchError::ZydisRelocationFailed);
    }
  }

  const std::size_t pad_len = alt.instr_len - alt.repl_len;
  if (pad_len > 0) {
    InstructionDecoder::pad_with_nops(std::span{payload.data() + alt.repl_len, pad_len}, target + alt.repl_len);
  }

  klib::memcpy(dest, payload.data(), alt.instr_len);
  return {};
}

void apply_all_boot_patches() noexcept {
  WpDisable wp_guard;

  const CpuInfo *cpu = profile_manager.get_current();

  for (const auto *alt = __start_altinstructions; alt < __stop_altinstructions; ++alt) {
    if (!cpu->has(alt->feature_flag)) {
      continue;
    }

    const auto target_addr =
        reinterpret_cast<std::uintptr_t>(&alt->instr_offset) + static_cast<std::intptr_t>(alt->instr_offset);

    bool higher_prio_patch_exists = false;
    for (const auto *comp = __start_altinstructions; comp < __stop_altinstructions; ++comp) {
      if (comp == alt) {
        continue;
      }

      const auto comp_target =
          reinterpret_cast<uintptr_t>(&comp->instr_offset) + static_cast<std::intptr_t>(comp->instr_offset);
      if (comp_target == target_addr && cpu->has(comp->feature_flag)) {
        if (comp->priority > alt->priority || (comp->priority == alt->priority && comp < alt)) {
          higher_prio_patch_exists = true;
          break;
        }
      }
    }

    if (higher_prio_patch_exists) {
      continue;
    }

    [[maybe_unused]] auto res = BootPatcher::apply_patch(*alt);
  }

  for (const auto *key = __start_static_keys; key < __stop_static_keys; ++key) {
    const auto key_def_addr =
        reinterpret_cast<std::uintptr_t>(&key->key_def_offset) + static_cast<std::int64_t>(key->key_def_offset);
    const auto *key_def = reinterpret_cast<const StaticKeyDef *>(key_def_addr);

    if (!key_def->enabled) {
      continue;
    }

    const auto nop_addr =
        reinterpret_cast<std::uintptr_t>(&key->nop_offset) + static_cast<std::int64_t>(key->nop_offset);
    const auto target_addr =
        reinterpret_cast<std::uintptr_t>(&key->target_offset) + static_cast<std::int64_t>(key->target_offset);

    const std::int64_t disp64 = static_cast<std::int64_t>(target_addr) - static_cast<std::int64_t>(nop_addr + 5);

    constexpr std::int64_t MAX_REL32 = std::numeric_limits<std::int32_t>::max();
    constexpr std::int64_t MIN_REL32 = std::numeric_limits<std::int32_t>::min();

    if (disp64 < MIN_REL32 || disp64 > MAX_REL32) {
      utils::logger::fatal("Static Key target out of 32-bit bounds!\n");
      continue;
    }

    auto *dest = reinterpret_cast<std::uint8_t *>(target_addr);

    if (disp64 >= -128 && disp64 <= 127) {
      std::int8_t disp8 = static_cast<std::int8_t>(target_addr - (nop_addr + 2));

      dest[0] = 0xEB; // JMP rel8
      dest[1] = disp8;

      // Multi-byte NOP
      dest[2] = 0x0F;
      dest[3] = 0x1F;
      dest[4] = 0x00;
    } else {
      dest[0] = 0xE9; // JMP rel32

      const std::int32_t disp = static_cast<std::int32_t>(disp64);
      klib::memcpy(dest + 1, &disp, sizeof(disp));
    }
  }
}
} // namespace kernel::hw::patcher