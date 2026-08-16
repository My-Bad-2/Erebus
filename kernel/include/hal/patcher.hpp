#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

#include "cpu_info.hpp"

namespace kernel::hw::patcher {
template <typename T>
concept CodePointer = std::is_pointer_v<T> || std::is_integral_v<T>;

enum class PatchError {
  InstructionBoundaryMismatch,
  TargetTooSmallForTrampoline,
  ZydisRelocationFailed,
  ZydisConversionFailed,
  IBTViolation // triggered if ENDBR64 is missing from a replacement
};

using PatchResult = std::expected<void, PatchError>;
constexpr std::uint32_t FEATURE_ALWAYS = 0xFFFFFFFF;

enum class PatchFlags : std::uint8_t { INLINE, OUT_OF_TRAMPOLINE, STRIP_PREFIX };

struct [[gnu::packed]] AltInstr {
  std::int32_t instr_offset; // Relative offset to original code in .text
  std::int32_t repl_offset;  // Relative offset to replacement code
  Feature feature_flag;
  std::uint8_t instr_len;
  std::uint8_t repl_len;
  std::uint8_t priority;
  PatchFlags flags;
};

struct StaticKeyDef {
  bool enabled;
};

struct [[gnu::packed]] StaticKeyEntry {
  std::int32_t nop_offset;
  std::int32_t target_offset;
  std::int32_t key_def_offset;
};

struct [[gnu::packed]] ExceptionTableEntry {
  std::int32_t insn_offset;
  std::int32_t fixup_offset;
};

class InstructionDecoder {
public:
  static void pad_with_nops(std::span<std::uint8_t> buffer, std::uintptr_t target_addr) noexcept;
  [[nodiscard]] static bool verify_boundaries(const std::uint8_t *code, std::uint8_t expected_len) noexcept;
  [[nodiscard]] static bool relocate_rip_instructions(std::span<std::uint8_t> payload, std::uintptr_t old_rip,
                                                      std::uintptr_t new_rip) noexcept;
};

class ExceptionTable {
public:
  static void sync_trampoline(std::uintptr_t old_rip, std::uintptr_t new_rip) noexcept;
};

class BootPatcher {
public:
  [[nodiscard]] static PatchResult apply_patch(const AltInstr &alt) noexcept;

private:
  static bool validate_ibt(std::span<const std::uint8_t> payload, const std::uint8_t *target) noexcept;
};

void apply_all_boot_patches() noexcept;
} // namespace kernel::hw::patcher
