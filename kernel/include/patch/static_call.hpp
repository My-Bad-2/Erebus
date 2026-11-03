/// @file static_call.hpp
/// @brief Calls through a patchable trampoline, pointed once at boot at the best implementation
///        the CPU supports.
///
/// A static call looks like an ordinary function to its callers. Behind it sits an 8-byte
/// trampoline, `jmp rel32` to the default implementation followed by `int3` padding.
/// StaticCallPatcher::apply_patches() rewrites that jump to the alternative with the highest
/// priority whose feature requirements the boot CPU meets, or to a plain `ret`. Every call stays
/// a direct call plus a direct jump, so there is no indirect branch to mispredict.
///
/// @code
/// // In a header:
/// DECLARE_STATIC_CALL(void, cpu_relax_impl, std::uint32_t, std::uint64_t);
///
/// // In exactly one source file:
/// DEFINE_STATIC_CALL(cpu_relax_impl, pause);
/// REGISTER_STATIC_CALL_ALT(cpu_relax_impl, tpause, Feature::WAITPKG);
/// @endcode
///
/// Until the patcher has run, every static call reaches its default.
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "core/errors.hpp"
#include "hal/smp/cpu_info.hpp"

#define SC_CONCAT_INNER(a, b) a##b
#define SC_CONCAT(a, b) SC_CONCAT_INNER(a, b)

/// @brief Assembler name of the trampoline behind static call `name`.
#define SC_TRAMPOLINE(name) "__static_call_tramp_" #name

// Each trampoline is rewritten with one aligned 8-byte store, so it must not cross an 8-byte
// boundary; 16-byte alignment also gives it a fetch block of its own. The section is named like
// a C identifier because lld never folds such sections, and --icf would otherwise merge two
// trampolines that happen to jump to the same function.
// clang-format off
#define SC_TRAMPOLINE_BEGIN(name)                                               \
  ".pushsection static_call_tramps, \"ax\", @progbits\n\t"                      \
  ".balign 16, 0xcc\n\t"                                                        \
  ".globl " SC_TRAMPOLINE(name) "\n\t"                                          \
  ".hidden " SC_TRAMPOLINE(name) "\n\t"                                         \
  ".type " SC_TRAMPOLINE(name) ", @function\n"                                  \
  SC_TRAMPOLINE(name) ":\n\t"

#define SC_TRAMPOLINE_END(name)                                                 \
  ".size " SC_TRAMPOLINE(name) ", 8\n\t"                                        \
  ".popsection\n\t"

// `jmp rel32` to the operand, and `ret; nopl 0(%rax)`, each padded to 8 bytes with int3.
#define SC_JUMP_BYTES ".byte 0xe9\n\t.long %P0 - . - 4\n\t.byte 0xcc, 0xcc, 0xcc\n\t"
#define SC_RETURN_BYTES ".byte 0xc3, 0x0f, 0x1f, 0x40, 0x00, 0xcc, 0xcc, 0xcc\n\t"
// clang-format on

/// @brief Declares static call `name`, called like `ret_type name(...)`.
#define DECLARE_STATIC_CALL(ret_type, name, ...) \
  [[gnu::visibility("hidden")]] ret_type name(__VA_ARGS__) noexcept __asm__(SC_TRAMPOLINE(name))

/// @brief Emits the trampoline of `name`, jumping to `default_fn` until patched.
///
/// The jump is spelled out byte by byte so that it is always the 5-byte `E9 rel32` form the
/// patcher knows how to rewrite. The asm needs the address of `default_fn` as an operand, and
/// operands are only allowed inside a function: the function itself is empty, and the linker
/// script discards its section.
#define DEFINE_STATIC_CALL(name, default_fn)                                                      \
  static_assert(std::same_as<decltype(&name), decltype(&default_fn)>,                             \
                "DEFINE_STATIC_CALL: " #default_fn " does not match the signature of " #name);    \
  [[gnu::used, gnu::noinline, gnu::no_instrument_function, gnu::section(".discard.static_call")]] \
  static void SC_CONCAT(__sc_def_, __COUNTER__)() noexcept {                                      \
    asm volatile(SC_TRAMPOLINE_BEGIN(name) SC_JUMP_BYTES SC_TRAMPOLINE_END(name)                  \
                 :                                                                                \
                 : "X"(&default_fn));                                                             \
  }

/// @brief Emits the trampoline of `name` as a bare `ret`, for a call that does nothing unless an
///        alternative applies. Only for calls returning `void`.
#define DEFINE_STATIC_CALL_NOP(name) \
  asm(SC_TRAMPOLINE_BEGIN(name) SC_RETURN_BYTES SC_TRAMPOLINE_END(name))

// The entries live in a section named like a C identifier for the same reason as the
// trampolines, and are kept by the linker script although nothing refers to them by name.
#define SC_TABLE_ENTRY(kind)                                                              \
  [[gnu::used, gnu::section("static_call_table")]] static constinit const auto SC_CONCAT( \
      kind, __COUNTER__)

/// @brief Registers `alt_fn` as an implementation of `name` for CPUs that meet every listed
///        requirement. Up to three, each a `hw::Feature` the CPU must have or a `!hw::Feature`
///        it must lack.
///
/// Among the alternatives the boot CPU qualifies for, the highest `prio` wins; on a tie, the one
/// the linker placed last.
#define REGISTER_STATIC_CALL_ALT_PRIO(name, alt_fn, prio, ...)                                   \
  static_assert(std::same_as<decltype(&name), decltype(&alt_fn)>,                                \
                "REGISTER_STATIC_CALL_ALT: " #alt_fn " does not match the signature of " #name); \
  SC_TABLE_ENTRY(__sc_alt_) = ::kernel::patch::make_static_call_entry(                           \
      &name, &alt_fn, prio, ::kernel::patch::make_feature_pack(__VA_ARGS__))

/// @brief REGISTER_STATIC_CALL_ALT_PRIO() at the default priority, 100.
#define REGISTER_STATIC_CALL_ALT(name, alt_fn, ...) \
  REGISTER_STATIC_CALL_ALT_PRIO(name, alt_fn, 100, __VA_ARGS__)

/// @brief Turns `name` into an immediate return on CPUs that meet the listed requirements. Only
///        for calls returning `void`. Competes with the alternatives by priority.
#define DISABLE_STATIC_CALL_ON_FEATURE_PRIO(name, prio, ...)                            \
  SC_TABLE_ENTRY(__sc_dis_) = ::kernel::patch::make_static_call_entry<decltype(&name)>( \
      &name, nullptr, prio, ::kernel::patch::make_feature_pack(__VA_ARGS__))

/// @brief DISABLE_STATIC_CALL_ON_FEATURE_PRIO() at the default priority, 100.
#define DISABLE_STATIC_CALL_ON_FEATURE(name, ...) \
  DISABLE_STATIC_CALL_ON_FEATURE_PRIO(name, 100, __VA_ARGS__)

namespace kernel::patch {
/// @brief How many feature requirements one alternative may list.
inline constexpr std::size_t MAX_FEATURE_REQUIREMENTS = 3;

/// @brief One feature an alternative needs present, or, negated, absent.
struct FeatureReq {
  hw::Feature feature = hw::Feature::ALWAYS;
  bool negated = false;

  consteval FeatureReq() noexcept = default;
  consteval FeatureReq(const hw::Feature f) noexcept : feature{f} {}
  consteval FeatureReq(const hw::Feature f, const bool neg) noexcept : feature{f}, negated{neg} {}
};

/// @brief The requirements of one alternative, as stored in its table entry.
struct FeaturePack {
  std::array<hw::Feature, MAX_FEATURE_REQUIREMENTS> list{};
  std::uint8_t count = 0;
  /// Bit `i` set means `list[i]` must be absent.
  std::uint8_t negate_mask = 0;

  /// @brief Whether `info` meets every requirement. hw::Feature::ALWAYS always holds.
  [[nodiscard]] constexpr bool is_met_by(const hw::CpuInfo& info) const noexcept {
    for (std::uint8_t i = 0; i < count; ++i) {
      const bool present = list[i] == hw::Feature::ALWAYS || info.has(list[i]);
      const bool must_be_absent = (negate_mask & (1u << i)) != 0;

      if (present == must_be_absent) {
        return false;
      }
    }

    return true;
  }
};

template <std::convertible_to<FeatureReq>... Reqs>
  requires(sizeof...(Reqs) >= 1 && sizeof...(Reqs) <= MAX_FEATURE_REQUIREMENTS)
[[nodiscard]] consteval FeaturePack make_feature_pack(const Reqs... reqs) noexcept {
  const std::array<FeatureReq, sizeof...(Reqs)> parsed{FeatureReq{reqs}...};
  FeaturePack pack{};
  pack.count = static_cast<std::uint8_t>(sizeof...(Reqs));

  for (std::size_t i = 0; i < parsed.size(); ++i) {
    pack.list[i] = parsed[i].feature;
    if (parsed[i].negated) {
      pack.negate_mask |= static_cast<std::uint8_t>(1u << i);
    }
  }

  return pack;
}

/// @brief A table entry with typed function pointers, which is what a constant initializer can
///        store. The patcher reads it back as a StaticCallEntry.
template <typename FnPtr>
struct TypedStaticCallEntry {
  FnPtr site;
  FnPtr target;
  FeaturePack requirements;
  std::uint16_t priority;
};

/// @brief One entry of the static call table: when `requirements` hold, point the trampoline at
///        `site` to `target`, or make it return straight away if `target` is null.
struct StaticCallEntry {
  std::uintptr_t site;
  std::uintptr_t target;
  FeaturePack requirements;
  std::uint16_t priority;
};

template <typename FnPtr>
[[nodiscard]] consteval TypedStaticCallEntry<FnPtr> make_static_call_entry(
    const FnPtr site, const FnPtr target, const std::uint16_t priority,
    const FeaturePack requirements) noexcept {
  static_assert(sizeof(FnPtr) == sizeof(std::uintptr_t));
  static_assert(sizeof(TypedStaticCallEntry<FnPtr>) == sizeof(StaticCallEntry) &&
                    alignof(TypedStaticCallEntry<FnPtr>) == alignof(StaticCallEntry),
                "the patcher would misread the static call table");
  return {.site = site, .target = target, .requirements = requirements, .priority = priority};
}

/// @brief Applies the static call table to the trampolines.
class StaticCallPatcher {
 public:
  /// @brief Points every trampoline at its best alternative for the boot CPU.
  ///
  /// Runs once, on the BSP, before the APs are started. Each trampoline is rewritten with a
  /// single 8-byte store, so one that is in use, by an interrupt handler say, jumps either to its
  /// old target or to its new one. Fails, leaving the remaining trampolines as they were, if an
  /// entry points outside the trampolines or a trampoline does not hold what the macros emit.
  static std::expected<void, Error> apply_patches() noexcept;
};
}  // namespace kernel::patch

namespace kernel::hw {
/// @brief Negates a requirement: `!Feature::HYPERVISOR` means "not under a hypervisor".
[[nodiscard]] consteval patch::FeatureReq operator!(const Feature f) noexcept {
  return patch::FeatureReq{f, true};
}
}  // namespace kernel::hw
