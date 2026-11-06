/// @file fpu_registers.hpp
/// @brief Reads and writes the FPU and vector registers directly, for the tests that check they
///        come through a save or a thread switch unharmed.
///
/// All of it has to run inside a utils::FpuGuard: the kernel is built not to touch these.
#pragma once

#include <array>
#include <cstdint>

#include "hal/fpu.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "ktest.hpp"

namespace kernel::testing {
/// @brief The widest vector registers the kernel enabled.
enum class VectorWidth : std::uint8_t { Xmm, Ymm, Zmm };

[[nodiscard]] inline VectorWidth vector_width() noexcept {
  if (!hw::percpu::self()->info->has<hw::Feature::XSAVE>()) {
    return VectorWidth::Xmm;
  }

  const hw::XCR0 xcr0 = hw::read::xcr0();
  if (xcr0.get_zmm_hi256()) {
    return VectorWidth::Zmm;
  }

  return xcr0.get_avx() ? VectorWidth::Ymm : VectorWidth::Xmm;
}

/// @brief Every register the tests care about. What the CPU does not have stays zero.
struct alignas(64) RegisterFile {
  static constexpr std::size_t WORDS_PER_VECTOR = 8;

  std::array<std::uint64_t, 32 * WORDS_PER_VECTOR> vectors{};
  std::array<std::uint16_t, 7>
      opmasks{};  // k1 to k7; k0 is special, and 16 bits is all of AVX-512F
  std::uint32_t mxcsr{hw::fpu::DEFAULT_MXCSR};
  std::uint16_t fcw{hw::fpu::DEFAULT_FCW};

  [[nodiscard]] constexpr bool operator==(const RegisterFile&) const noexcept = default;
};

/// @brief Different values in every register the CPU has, different for every @p seed.
[[nodiscard]] inline RegisterFile make_pattern(const std::uint64_t seed) noexcept {
  const VectorWidth width = vector_width();
  const std::size_t registers = width == VectorWidth::Zmm ? 32 : 16;
  const std::size_t words = width == VectorWidth::Zmm ? 8 : (width == VectorWidth::Ymm ? 4 : 2);

  RegisterFile file{};
  Rng rng{seed};

  for (std::size_t reg = 0; reg < registers; ++reg) {
    for (std::size_t word = 0; word < words; ++word) {
      file.vectors[reg * RegisterFile::WORDS_PER_VECTOR + word] = rng.next();
    }
  }

  if (width == VectorWidth::Zmm) {
    for (auto& mask : file.opmasks) {
      mask = static_cast<std::uint16_t>(rng.next());
    }
  }

  // The rounding control of both, which is harmless to change and has to come back like the rest.
  const auto rounding = static_cast<std::uint32_t>(seed & 3);
  file.mxcsr |= rounding << 13;
  file.fcw = static_cast<std::uint16_t>(file.fcw | (rounding << 10));
  return file;
}

// clang-format off
#define KTEST_FOR_LOW_VECTORS(fn)                                                   \
  fn(0, 0) fn(1, 64) fn(2, 128) fn(3, 192) fn(4, 256) fn(5, 320) fn(6, 384) fn(7, 448)      \
  fn(8, 512) fn(9, 576) fn(10, 640) fn(11, 704) fn(12, 768) fn(13, 832) fn(14, 896) fn(15, 960)

#define KTEST_FOR_HIGH_VECTORS(fn)                                                         \
  fn(16, 1024) fn(17, 1088) fn(18, 1152) fn(19, 1216) fn(20, 1280) fn(21, 1344) fn(22, 1408) \
  fn(23, 1472) fn(24, 1536) fn(25, 1600) fn(26, 1664) fn(27, 1728) fn(28, 1792) fn(29, 1856) \
  fn(30, 1920) fn(31, 1984)

#define KTEST_FOR_OPMASKS(fn) fn(1, 0) fn(2, 2) fn(3, 4) fn(4, 6) fn(5, 8) fn(6, 10) fn(7, 12)

#define KTEST_LOAD_XMM(n, offset) "movdqu " #offset "(%0), %%xmm" #n "\n\t"
#define KTEST_LOAD_YMM(n, offset) "vmovdqu " #offset "(%0), %%ymm" #n "\n\t"
#define KTEST_LOAD_ZMM(n, offset) "vmovdqu64 " #offset "(%0), %%zmm" #n "\n\t"
#define KTEST_LOAD_K(n, offset) "kmovw " #offset "(%1), %%k" #n "\n\t"

#define KTEST_STORE_XMM(n, offset) "movdqu %%xmm" #n ", " #offset "(%0)\n\t"
#define KTEST_STORE_YMM(n, offset) "vmovdqu %%ymm" #n ", " #offset "(%0)\n\t"
#define KTEST_STORE_ZMM(n, offset) "vmovdqu64 %%zmm" #n ", " #offset "(%0)\n\t"
#define KTEST_STORE_K(n, offset) "kmovw %%k" #n ", " #offset "(%1)\n\t"
// clang-format on

/// @brief Puts @p file into the CPU's registers.
inline void load_registers(const RegisterFile& file) noexcept {
  const std::uint64_t* vectors = file.vectors.data();
  const std::uint16_t* opmasks = file.opmasks.data();

  asm volatile(
      "ldmxcsr %0\n\t"
      "fldcw %1"
      : /* No Output */
      : "m"(file.mxcsr), "m"(file.fcw)
      : /* No Clobber */);

  switch (vector_width()) {
    case VectorWidth::Xmm:
      asm volatile(KTEST_FOR_LOW_VECTORS(KTEST_LOAD_XMM)
                   : /* No Output */
                   : "r"(vectors)
                   : "memory");
      break;
    case VectorWidth::Ymm:
      asm volatile(KTEST_FOR_LOW_VECTORS(KTEST_LOAD_YMM)
                   : /* No Output */
                   : "r"(vectors)
                   : "memory");
      break;
    case VectorWidth::Zmm:
      asm volatile(KTEST_FOR_LOW_VECTORS(KTEST_LOAD_ZMM) KTEST_FOR_HIGH_VECTORS(KTEST_LOAD_ZMM)
                       KTEST_FOR_OPMASKS(KTEST_LOAD_K)
                   : /* No Output */
                   : "r"(vectors), "r"(opmasks)
                   : "memory");
      break;
  }
}

/// @brief Copies the CPU's registers into @p file.
inline void store_registers(RegisterFile& file) noexcept {
  std::uint64_t* vectors = file.vectors.data();
  std::uint16_t* opmasks = file.opmasks.data();

  asm volatile(
      "stmxcsr %0\n\t"
      "fnstcw %1"
      : "=m"(file.mxcsr), "=m"(file.fcw)
      : /* No Input */
      : /* No Clobber */);

  switch (vector_width()) {
    case VectorWidth::Xmm:
      asm volatile(KTEST_FOR_LOW_VECTORS(KTEST_STORE_XMM)
                   : /* No Output */
                   : "r"(vectors)
                   : "memory");
      break;
    case VectorWidth::Ymm:
      asm volatile(KTEST_FOR_LOW_VECTORS(KTEST_STORE_YMM)
                   : /* No Output */
                   : "r"(vectors)
                   : "memory");
      break;
    case VectorWidth::Zmm:
      asm volatile(KTEST_FOR_LOW_VECTORS(KTEST_STORE_ZMM) KTEST_FOR_HIGH_VECTORS(KTEST_STORE_ZMM)
                       KTEST_FOR_OPMASKS(KTEST_STORE_K)
                   : /* No Output */
                   : "r"(vectors), "r"(opmasks)
                   : "memory");
      break;
  }
}
}  // namespace kernel::testing
