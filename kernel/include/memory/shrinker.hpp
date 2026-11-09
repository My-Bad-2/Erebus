/// @file shrinker.hpp
/// @brief Memory the kernel holds on to without needing it, and how reclaim gets it back.
///
/// A lot of free memory is not free as far as the PMM can tell: empty slabs a cache keeps for the
/// next burst, thread stacks kept for the next thread, and so on. Each of those registers a
/// Shrinker, and the reclaim daemon of a node asks all of them for memory when the node runs low.
///
/// The registry is what keeps the daemon in the memory layer while the caches it empties live in
/// the layers above it: they come to the daemon, so the daemon never has to know about them.
///
/// A daemon walks the registry in a read section of a sleepable RCU domain (utils/rcu/srcu.hpp)
/// and calls each shrinker from inside it, as shrinkers sleep. So the walk takes no lock, however
/// long the calls take, and a shrinker that is taken off waits until no walk can still call it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "utils/rcu/rcu_ptr.hpp"

namespace kernel::memory {
/// @brief Something that can give memory back on request. See the file comment.
class Shrinker {
 public:
  /// @brief Gives back whatever the owner can spare that came from NUMA node @p node_id.
  ///
  /// Called from a thread that can sleep, possibly from the daemons of several nodes at once.
  /// @return How many pages went back to the PMM.
  using Callback = std::size_t (*)(std::uint32_t node_id, void* context) noexcept;

  constexpr Shrinker(const Callback callback, void* context) noexcept
      : m_callback(callback), m_context(context) {}

  Shrinker(const Shrinker&) = delete;
  Shrinker& operator=(const Shrinker&) = delete;

 private:
  friend void register_shrinker(Shrinker& shrinker) noexcept;
  friend void unregister_shrinker(Shrinker& shrinker) noexcept;
  friend std::size_t run_shrinkers(std::uint32_t node_id) noexcept;

  Callback m_callback;
  void* m_context;

  // The next one in the registry. Walks follow it without a lock, so one that was taken off keeps
  // pointing on to the rest until unregister_shrinker() is done waiting.
  utils::rcu::Ptr<Shrinker> m_next{};
};

/// @brief Lets reclaim call @p shrinker from now on. It must stay put until it is unregistered.
void register_shrinker(Shrinker& shrinker) noexcept;

/// @brief Takes @p shrinker off the list, and waits for every walk of it that might still call
///        the shrinker. After that it is the caller's again.
/// @pre The caller can sleep, and is not a shrinker: a walk would be waiting for itself.
void unregister_shrinker(Shrinker& shrinker) noexcept;

/// @brief Asks every shrinker for memory of node @p node_id.
/// @pre The calling thread can sleep.
/// @return How many pages went back, all of them together.
std::size_t run_shrinkers(std::uint32_t node_id) noexcept;
}  // namespace kernel::memory
