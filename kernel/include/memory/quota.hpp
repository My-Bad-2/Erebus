/// @file quota.hpp
/// @brief Limits on the memory user code can make the kernel hold for it.
///
/// What user code asks the kernel to make, an endpoint, a CSpace, a shared memory object, takes
/// kernel memory, and nothing else would stop a process from asking until there is none left. So
/// each such object counts against a Quota, from the moment it is made until it is gone, and
/// making one past the limit fails with Error::QuotaExceeded instead.
///
/// A Quota belongs to a tree of processes: a process shares the one of the process that made it,
/// so starting more processes is no way around it, and only one made by the kernel gets a new one
/// (see proc::Process::create()). An object made for a process counts against that quota even
/// once it is handed to someone else, and until it is gone, which can be after the process is: so
/// a quota is counted, and goes with the last reference to it, whether that is the process's or
/// an object's.
///
/// Every tree's quota is under the root one, and whatever it is charged, so is the root. A tree
/// may take half of RAM, and all of them together three quarters: no tree can starve the others
/// on its own, and together they leave the kernel what it needs for itself, its page tables, heap
/// and caches, which nobody is charged for.
///
/// What counts is what the memory may come to be: an object's own size, and for memory that is
/// filled in on demand the whole of it, as it can come to take that much without asking again.
/// So a quota is a limit on commitments, the way a strict overcommit policy is, not on pages in
/// use right now.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include "core/errors.hpp"

namespace kernel::memory {
/// @brief How much may be charged to one tree of processes. See the file comment.
class Quota {
 public:
  /// @brief What all trees together may be charged on a machine with @p total_bytes of RAM.
  [[nodiscard]] static constexpr std::size_t root_limit_for(
      const std::size_t total_bytes) noexcept {
    return total_bytes / 4 * 3;
  }

  /// @brief What one tree may be charged on a machine with @p total_bytes of RAM.
  [[nodiscard]] static constexpr std::size_t tree_limit_for(
      const std::size_t total_bytes) noexcept {
    return total_bytes / 2;
  }

  /// @pre @p parent, if there is one, comes with a reference for this to keep.
  Quota(const std::size_t limit, Quota* const parent) noexcept : m_limit(limit), m_parent(parent) {}

  Quota(const Quota&) = delete;
  Quota& operator=(const Quota&) = delete;

  /// @brief A quota of @p limit bytes under @p parent, if there is one, with the one reference
  ///        there is. Null without memory.
  [[nodiscard]] static Quota* create(std::size_t limit, Quota* parent = nullptr) noexcept;

  /// @brief Makes the root quota, sized by how much RAM there is. Called once, by
  ///        memory::initialize(), when the PMM and the heap are up.
  static void initialize_root() noexcept;

  /// @brief The quota every tree is under. Null before initialize_root().
  [[nodiscard]] static Quota* root() noexcept;

  /// @brief A quota for a new tree of processes, under the root, with the one reference there is.
  ///        Null without memory.
  [[nodiscard]] static Quota* create_tree() noexcept;

  /// @brief One more reference. @pre The caller has one.
  void retain() noexcept { m_refs.fetch_add(1, std::memory_order_relaxed); }

  /// @brief Lets go of a reference. The last one frees the quota, from any context but an NMI.
  void release() noexcept;

  /// @brief Counts @p bytes more against the quota, and the ones above it. Safe anywhere,
  ///        spinlocks held or not.
  /// @return QuotaExceeded, with nothing counted anywhere, if that would take any of them past
  ///         its limit.
  [[nodiscard]] std::expected<void, Error> charge(std::size_t bytes) noexcept;

  /// @brief Gives back what charge() took, here and above. @pre That much was charged and not
  ///        given back yet.
  void uncharge(std::size_t bytes) noexcept;

  [[nodiscard]] std::size_t used() const noexcept { return m_used.load(std::memory_order_relaxed); }
  [[nodiscard]] std::size_t limit() const noexcept { return m_limit; }
  [[nodiscard]] Quota* parent() const noexcept { return m_parent; }

 private:
  [[nodiscard]] bool charge_here(std::size_t bytes) noexcept;
  void uncharge_here(std::size_t bytes) noexcept;

  const std::size_t m_limit;
  Quota* const m_parent;  ///< With a reference.
  std::atomic<std::size_t> m_used{0};
  std::atomic<std::uint32_t> m_refs{1};
};

/// @brief An owned reference to a Quota, which may be none. Move-only.
class QuotaRef {
  Quota* m_quota{nullptr};

 public:
  constexpr QuotaRef() noexcept = default;

  /// @brief Takes over the reference @p quota comes with.
  [[nodiscard]] static QuotaRef adopt(Quota* quota) noexcept {
    QuotaRef ref;
    ref.m_quota = quota;
    return ref;
  }

  /// @brief Takes a reference of its own. @pre The caller has one.
  [[nodiscard]] static QuotaRef share(Quota* quota) noexcept {
    if (quota != nullptr) {
      quota->retain();
    }

    return adopt(quota);
  }

  QuotaRef(QuotaRef&& other) noexcept : m_quota(std::exchange(other.m_quota, nullptr)) {}

  QuotaRef& operator=(QuotaRef&& other) noexcept {
    if (this != &other) {
      reset();
      m_quota = std::exchange(other.m_quota, nullptr);
    }

    return *this;
  }

  QuotaRef(const QuotaRef&) = delete;
  QuotaRef& operator=(const QuotaRef&) = delete;

  ~QuotaRef() noexcept { reset(); }

  void reset() noexcept {
    if (Quota* quota = std::exchange(m_quota, nullptr)) {
      quota->release();
    }
  }

  [[nodiscard]] Quota* get() const noexcept { return m_quota; }
};

/// @brief Bytes counted against a quota for as long as this has them, with a reference to it.
///
/// What an object holds for whatever it takes, and gives back when it goes. Without a quota it
/// counts nothing and everything succeeds: the kernel's own objects are charged to nobody.
class QuotaCharge {
 public:
  constexpr QuotaCharge() noexcept = default;

  /// @brief Charges @p bytes to @p quota, which may be null for nobody.
  /// @return QuotaExceeded if the quota has no room for them.
  [[nodiscard]] static std::expected<QuotaCharge, Error> take(Quota* quota,
                                                              std::size_t bytes) noexcept;

  QuotaCharge(QuotaCharge&& other) noexcept
      : m_quota(std::move(other.m_quota)),
        m_bytes(other.m_bytes.exchange(0, std::memory_order_relaxed)) {}

  QuotaCharge& operator=(QuotaCharge&& other) noexcept {
    if (this != &other) {
      reset();
      m_quota = std::move(other.m_quota);
      m_bytes.store(other.m_bytes.exchange(0, std::memory_order_relaxed),
                    std::memory_order_relaxed);
    }

    return *this;
  }

  QuotaCharge(const QuotaCharge&) = delete;
  QuotaCharge& operator=(const QuotaCharge&) = delete;

  ~QuotaCharge() noexcept { reset(); }

  /// @brief Charges @p bytes more, for something that grows. Several threads may at once.
  /// @return QuotaExceeded, with nothing more charged, past the limit.
  [[nodiscard]] std::expected<void, Error> grow(std::size_t bytes) noexcept;

  /// @brief Gives back @p bytes of what grow() took, for growth that did not happen after all.
  void shrink(std::size_t bytes) noexcept;

  /// @brief Gives everything back, and lets go of the quota.
  void reset() noexcept;

  [[nodiscard]] Quota* quota() const noexcept { return m_quota.get(); }
  [[nodiscard]] std::size_t bytes() const noexcept {
    return m_bytes.load(std::memory_order_relaxed);
  }

 private:
  QuotaRef m_quota;
  std::atomic<std::size_t> m_bytes{0};
};
}  // namespace kernel::memory
