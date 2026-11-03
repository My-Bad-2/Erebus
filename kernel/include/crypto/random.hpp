/// @file random.hpp
/// @brief The kernel's random numbers: unpredictable, and safe to use for keys and for address
///        space layout randomization.
///
/// Entropy from the CPU's hardware RNG, timing jitter and boot data goes into a BLAKE2b pool.
/// The pool seeds a base ChaCha20 generator, which hands each CPU a key for a generator of its
/// own, so that the common case takes no lock. The base generator is reseeded from the pool
/// every minute, and every CPU picks up the new generation on its next request.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "crypto/chacha20.hpp"

namespace kernel::crypto {
/// @brief The per-CPU part of the generator. Lives in PerCpu, and only random.cpp touches it,
///        with interrupts off.
struct CpuRandomState {
  ChaChaRng generator{};
  /// The base generation this CPU's key came from; 0 until it first asks for random bytes.
  std::uint64_t generation = 0;
};

namespace random {
/// @brief Seeds the generator. Called once on the BSP, once per-CPU data is set up and before
///        anything asks for random numbers; asking earlier seeds it on the spot.
void initialize() noexcept;

/// @brief Mixes `data` into the entropy pool, to reach the generator at the next reseed.
///
/// Anything may go in: input an attacker knows or even chooses cannot make the output any
/// worse.
void add_entropy(std::span<const std::byte> data) noexcept;

/// @brief Fills `out` with random bytes. From any context but NMI.
void fill(std::span<std::byte> out) noexcept;

/// @brief A random value of any trivially copyable type, every bit of it random.
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] T get() noexcept {
  T value;
  fill(std::as_writable_bytes(std::span{&value, 1}));
  return value;
}

/// @brief A uniformly distributed value in [0, `bound`), without the bias of a plain `% bound`.
///        `bound` must not be 0.
[[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept;

/// @brief Whether the seed included output of the CPU's hardware RNG. Without it the generator
///        rests on timing jitter and boot data alone, which may well be guessable.
[[nodiscard]] bool has_hardware_seed() noexcept;
}  // namespace random
}  // namespace kernel::crypto
