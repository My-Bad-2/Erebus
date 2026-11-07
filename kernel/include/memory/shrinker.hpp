/// @file shrinker.hpp
/// @brief Memory the kernel holds on to without needing it, and how reclaim gets it back.
///
/// A lot of free memory is not free as far as the PMM can tell: empty slabs a cache keeps for the
/// next burst, thread stacks kept for the next thread, and so on. Each of those registers a
/// Shrinker, and the reclaim daemon of a node asks all of them for memory when the node runs low.
///
/// The registry is what keeps the daemon in the memory layer while the caches it empties live in
/// the layers above it: they come to the daemon, so the daemon never has to know about them.
#pragma once

#include <cstddef>
#include <cstdint>

#include "utils/linked_list.hpp"

namespace kernel::memory {
/// @brief Something that can give memory back on request. See the file comment.
class Shrinker : public utils::SListNode<Shrinker> {
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
  friend std::size_t run_shrinkers(std::uint32_t node_id) noexcept;
  friend void unregister_shrinker(Shrinker& shrinker) noexcept;

  Callback m_callback;
  void* m_context;

  // How many daemons are in the callback right now, under the registry's lock. It keeps the
  // shrinker on the list for them, and unregister_shrinker() waits for it.
  std::uint32_t m_running{0};
};

/// @brief Lets reclaim call @p shrinker from now on. It must stay put until it is unregistered.
void register_shrinker(Shrinker& shrinker) noexcept;

/// @brief Takes @p shrinker off the list, and waits for every call to it that is still going on.
/// @pre The caller is not one of those calls, and can sleep.
void unregister_shrinker(Shrinker& shrinker) noexcept;

/// @brief Asks every shrinker for memory of node @p node_id.
/// @pre The calling thread can sleep.
/// @return How many pages went back, all of them together.
std::size_t run_shrinkers(std::uint32_t node_id) noexcept;
}  // namespace kernel::memory
