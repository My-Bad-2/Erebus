#pragma once

#include <Zydis/Zydis.h>

#include <cstddef>
#include <cstdint>
#include <expected>

#include "core/errors.hpp"
#include "hal/smp/cpu_info.hpp"
#include "memory/address.hpp"

#define SC_CONCAT_INNER(a, b) a##b
#define SC_CONCAT(a, b) SC_CONCAT_INNER(a, b)

#define DECLARE_STATIC_CALL(ret_type, name, ...)                             \
  [[gnu::visibility("hidden")]] ret_type name(__VA_ARGS__) noexcept __asm__( \
      "__static_call_tramp_" #name)

#define DEFINE_STATIC_CALL(name, default_fn)                                            \
  static_assert(std::same_as<decltype(&name), decltype(&default_fn)>,                   \
                "DEFINE_STATIC_CALL: default_fn signature mismatch for " #name);        \
  [[gnu::used, gnu::noinline, gnu::no_instrument_function, gnu::section(".init.text")]] \
  static void SC_CONCAT(__sc_def_, __COUNTER__)() noexcept {                            \
    asm volatile(                                                                       \
        ".pushsection .text.static_calls, \"ax\", @progbits\n\t"                        \
        ".balign 16\n\t"                                                                \
        ".global __static_call_tramp_" #name                                            \
        "\n\t"                                                                          \
        ".hidden __static_call_tramp_" #name                                            \
        "\n\t"                                                                          \
        ".type __static_call_tramp_" #name                                              \
        ", @function\n\t"                                                               \
        "__static_call_tramp_" #name                                                    \
        ":\n\t"                                                                         \
        "jmp %P0\n\t"                                                                   \
        ".byte 0xcc, 0xcc, 0xcc\n\t"                                                    \
        ".size __static_call_tramp_" #name                                              \
        ", 8\n\t"                                                                       \
        ".popsection\n\t"                                                               \
        :                                                                               \
        : "X"(&default_fn));                                                            \
  }

#define DEFINE_STATIC_CALL_NOP(name)                           \
  asm(".pushsection .text.static_calls, \"ax\", @progbits\n\t" \
      ".balign 16\n\t"                                         \
      ".global __static_call_tramp_" #name                     \
      "\n\t"                                                   \
      ".hidden __static_call_tramp_" #name                     \
      "\n\t"                                                   \
      ".type __static_call_tramp_" #name                       \
      ", @function\n\t"                                        \
      "__static_call_tramp_" #name                             \
      ":\n\t"                                                  \
      ".byte 0xc3\n\t"                                         \
      ".byte 0x0f, 0x1f, 0x40, 0x00\n\t"                       \
      ".byte 0xcc, 0xcc, 0xcc\n\t"                             \
      ".size __static_call_tramp_" #name                       \
      ", 8\n\t"                                                \
      ".popsection\n\t")

#define REGISTER_STATIC_CALL_ALT_PRIO(name, alt_fn, prio, ...)                             \
  static_assert(std::same_as<decltype(&name), decltype(&alt_fn)>,                          \
                "REGISTER_STATIC_CALL_ALT: alt_fn signature mismatch for " #name);         \
  [[gnu::used, gnu::section(".init.static_calls"), gnu::aligned(8)]]                       \
  static constinit const ::kernel::patch::TypedStaticCallEntry<decltype(&name)> SC_CONCAT( \
      __sc_alt_, __COUNTER__) = {                                                          \
      .site = &name,                                                                       \
      .target = &alt_fn,                                                                   \
      .features = ::kernel::patch::make_feature_pack(__VA_ARGS__).list,                    \
      .priority = static_cast<std::uint16_t>(prio),                                        \
      .num_features = ::kernel::patch::make_feature_pack(__VA_ARGS__).count,               \
      .negate_mask = ::kernel::patch::make_feature_pack(__VA_ARGS__).negate_mask,          \
  }

#define REGISTER_STATIC_CALL_ALT(name, alt_fn, ...) \
  REGISTER_STATIC_CALL_ALT_PRIO(name, alt_fn, 100, __VA_ARGS__)

#define DISABLE_STATIC_CALL_ON_FEATURE_PRIO(name, prio, ...)                               \
  [[gnu::used, gnu::section(".init.static_calls"), gnu::aligned(8)]]                       \
  static constinit const ::kernel::patch::TypedStaticCallEntry<decltype(&name)> SC_CONCAT( \
      __sc_dis_, __COUNTER__) = {                                                          \
      .site = &name,                                                                       \
      .target = nullptr,                                                                   \
      .features = ::kernel::patch::make_feature_pack(__VA_ARGS__).list,                    \
      .priority = static_cast<std::uint16_t>(prio),                                        \
      .num_features = ::kernel::patch::make_feature_pack(__VA_ARGS__).count,               \
      .negate_mask = ::kernel::patch::make_feature_pack(__VA_ARGS__).negate_mask,          \
  }

#define DISABLE_STATIC_CALL_ON_FEATURE(name, ...) \
  DISABLE_STATIC_CALL_ON_FEATURE_PRIO(name, 100, __VA_ARGS__)

namespace kernel::patch {
inline constexpr std::size_t kMaxLayeredFeatures = 3;

struct FeatureReq {
  hw::Feature feature = hw::Feature::NONE;
  bool negated{false};

  consteval FeatureReq() noexcept = default;
  consteval FeatureReq(hw::Feature f) noexcept : feature{f} {}
  consteval FeatureReq(hw::Feature f, bool neg) noexcept : feature{f}, negated{neg} {}
};

struct FeaturePack {
  std::array<hw::Feature, kMaxLayeredFeatures> list{};
  std::uint8_t count = 0;
  std::uint8_t negate_mask = 0;  // Bit i == 1 means features[i] MUST NOT be present
};

template <std::convertible_to<FeatureReq>... Reqs>
  requires(sizeof...(Reqs) >= 1 && sizeof...(Reqs) <= kMaxLayeredFeatures)
[[nodiscard]] consteval auto make_feature_pack(Reqs... reqs) noexcept -> FeaturePack {
  const std::array<FeatureReq, sizeof...(Reqs)> parsed{FeatureReq{reqs}...};
  FeaturePack pack{};
  pack.count = static_cast<std::uint8_t>(sizeof...(Reqs));

  for (std::size_t i = 0; i < parsed.size(); ++i) {
    pack.list[i] = parsed[i].feature;
    if (parsed[i].negated) pack.negate_mask |= static_cast<std::uint8_t>(1u << i);
  }

  return pack;
}

struct [[gnu::aligned(8)]] StaticCallEntry {
  const void* site;    // Address of __static_call_tramp_<name>
  const void* target;  // Address of alt_fn (nullptr = disable/NOP)
  std::array<hw::Feature, kMaxLayeredFeatures> features;
  std::uint16_t priority;
  std::uint8_t num_features;
  std::uint8_t negate_mask;

  [[nodiscard]] constexpr memory::VirtualAddress site_address() const noexcept {
    return memory::VirtualAddress(site);
  }

  [[nodiscard]] constexpr memory::VirtualAddress target_address() const noexcept {
    return memory::VirtualAddress(target);
  }

  [[nodiscard]] constexpr bool is_supported(const hw::CpuInfo* info) const noexcept {
    for (std::uint8_t i = 0; i < num_features; ++i) {
      const bool present = info->has(features[i]);
      const bool must_be_off = (negate_mask & (1u << i)) != 0;

      if (must_be_off ? present : !present) return false;
    }
    return true;
  }
};

template <typename FnPtr>
struct [[gnu::packed]] TypedStaticCallEntry {
  FnPtr site;
  FnPtr target;
  std::array<hw::Feature, kMaxLayeredFeatures> features;
  std::uint16_t priority = 100;
  std::uint8_t num_features = 0;
  std::uint8_t negate_mask = 0;
};

class StaticCallPatcher {
 public:
  static constexpr std::size_t kDirectBranchSize = 5;  // E8/E9 <rel32>
  static constexpr std::size_t kMaxSiteSize = 6;       // FF 25 <disp32>

  static auto apply_patches() noexcept -> std::expected<void, Error>;

 private:
  static auto patch_site(const ZydisDecoder& decoder, const StaticCallEntry& entry,
                         memory::VirtualAddress text_start, std::size_t text_size,
                         memory::VirtualAddress alias_base) noexcept -> std::expected<void, Error>;

  static void serialize_pipeline(const hw::CpuInfo* info) noexcept;
};
}  // namespace kernel::patch

namespace kernel::hw {
[[nodiscard]] consteval auto operator!(Feature f) noexcept -> patch::FeatureReq {
  return patch::FeatureReq{f, true};
}
}  // namespace kernel::hw