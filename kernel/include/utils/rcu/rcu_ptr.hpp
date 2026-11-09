/// @file rcu_ptr.hpp
/// @brief rcu::Ptr, a pointer that readers follow inside a read section and writers replace.
#pragma once

#include <atomic>
#include <concepts>
#include <type_traits>

#include "utils/rcu/rcu.hpp"

namespace kernel::utils::rcu {
/// @brief A read guard of any kind: rcu::ReadGuard, or anything that says it is one.
template <typename Guard>
concept ReadLockWitness = std::same_as<std::remove_cvref_t<Guard>, ReadGuard> ||
                          requires { std::remove_cvref_t<Guard>::is_rcu_read_guard_witness(); };

/// @brief A pointer to `T` for RCU readers and writers.
///
/// @code
/// rcu::Ptr<RouteEntry> g_default_route{nullptr};
///
/// // Reader:
/// {
///   rcu::ReadGuard rcu;
///   if (const RouteEntry* route = g_default_route.load(rcu)) {
///     use(route->gateway);
///   }
/// }
///
/// // Writer, with whatever keeps other writers out:
/// g_default_route.replace_and_retire(new RouteEntry{...});
/// @endcode
///
/// Publishing is a release, and loading an acquire, so a reader that finds an object finds it
/// initialized.
template <typename T>
class Ptr {
  std::atomic<T*> m_ptr{nullptr};

 public:
  constexpr Ptr() noexcept = default;
  constexpr explicit Ptr(T* initial) noexcept : m_ptr(initial) {}

  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;

  /// @brief What a reader may follow, until its guard goes.
  template <ReadLockWitness Guard>
  [[nodiscard, gnu::always_inline]] T* load(const Guard&) const noexcept {
    return m_ptr.load(std::memory_order_acquire);
  }

  /// @brief The pointer as a writer sees it, with other writers kept out by the writers' lock.
  [[nodiscard, gnu::always_inline]] T* load_writer_locked() const noexcept {
    return m_ptr.load(std::memory_order_relaxed);
  }

  /// @brief Publishes @p desired, which must be fully initialised by now.
  [[gnu::always_inline]] void store(T* desired) noexcept {
    m_ptr.store(desired, std::memory_order_release);
  }

  /// @brief Publishes @p desired and returns what it replaced, which the caller retires.
  [[nodiscard, gnu::always_inline]] T* exchange(T* desired) noexcept {
    return m_ptr.exchange(desired, std::memory_order_acq_rel);
  }

  [[nodiscard, gnu::always_inline]] bool compare_exchange_weak(T*& expected, T* desired) noexcept {
    return m_ptr.compare_exchange_weak(expected, desired, std::memory_order_acq_rel,
                                       std::memory_order_acquire);
  }

  [[nodiscard, gnu::always_inline]] bool compare_exchange_strong(T*& expected,
                                                                 T* desired) noexcept {
    return m_ptr.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
  }

  /// @brief Publishes @p desired and retires what it replaced (see rcu::retire()).
  ///
  /// The old object must have come from the heap, and nothing else may still point at it: from
  /// here on, only readers that found it already can see it.
  void replace_and_retire(T* desired) noexcept { retire(exchange(desired)); }
};
}  // namespace kernel::utils::rcu
