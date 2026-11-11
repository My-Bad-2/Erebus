#include "memory/quota.hpp"

#include <memory>

#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "utils/assert.hpp"
#include "utils/logger.hpp"

namespace kernel::memory {
namespace {
constinit Quota* g_root = nullptr;
}  // namespace

Quota* Quota::create(const std::size_t limit, Quota* const parent) noexcept {
  void* memory = heap::kmalloc(sizeof(Quota), alignof(Quota));
  if (memory == nullptr) [[unlikely]] {
    return nullptr;
  }

  if (parent != nullptr) {
    parent->retain();
  }

  return std::construct_at(static_cast<Quota*>(memory), limit, parent);
}

void Quota::initialize_root() noexcept {
  KASSERT(g_root == nullptr);
  const std::size_t total = pmm::total_bytes();
  g_root = create(root_limit_for(total));
  if (g_root == nullptr) {
    utils::logger::fatal("Quota: no memory for the root quota\n");
  }

  utils::logger::info("Quota: user memory may take {} MiB of {} MiB, one tree {} MiB\n",
                      g_root->limit() >> 20, total >> 20, tree_limit_for(total) >> 20);
}

Quota* Quota::root() noexcept { return g_root; }

Quota* Quota::create_tree() noexcept { return create(tree_limit_for(pmm::total_bytes()), g_root); }

void Quota::release() noexcept {
  if (m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    KASSERT(m_used.load(std::memory_order_relaxed) == 0);
    Quota* const parent = m_parent;
    std::destroy_at(this);
    heap::kfree(this);
    if (parent != nullptr) {
      parent->release();
    }
  }
}

bool Quota::charge_here(const std::size_t bytes) noexcept {
  std::size_t used = m_used.load(std::memory_order_relaxed);
  do {
    if (bytes > m_limit - used) [[unlikely]] {
      return false;
    }
  } while (!m_used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));

  return true;
}

void Quota::uncharge_here(const std::size_t bytes) noexcept {
  [[maybe_unused]] const std::size_t used = m_used.fetch_sub(bytes, std::memory_order_relaxed);
  KASSERT(used >= bytes);
}

std::expected<void, Error> Quota::charge(const std::size_t bytes) noexcept {
  for (auto* level = this; level != nullptr; level = level->m_parent) {
    if (!level->charge_here(bytes)) [[unlikely]] {
      for (auto* below = this; below != level; below = below->m_parent) {
        below->uncharge_here(bytes);
      }

      return std::unexpected(Error::QuotaExceeded);
    }
  }

  return {};
}

void Quota::uncharge(const std::size_t bytes) noexcept {
  for (Quota* level = this; level != nullptr; level = level->m_parent) {
    level->uncharge_here(bytes);
  }
}

std::expected<QuotaCharge, Error> QuotaCharge::take(Quota* const quota,
                                                    const std::size_t bytes) noexcept {
  QuotaCharge charge;
  if (quota == nullptr) {
    return charge;
  }

  if (const auto charged = quota->charge(bytes); !charged) [[unlikely]] {
    return std::unexpected(charged.error());
  }

  charge.m_quota = QuotaRef::share(quota);
  charge.m_bytes.store(bytes, std::memory_order_relaxed);
  return charge;
}

std::expected<void, Error> QuotaCharge::grow(const std::size_t bytes) noexcept {
  Quota* const quota = m_quota.get();
  if (quota == nullptr) {
    return {};
  }

  if (const auto charged = quota->charge(bytes); !charged) [[unlikely]] {
    return charged;
  }

  m_bytes.fetch_add(bytes, std::memory_order_relaxed);
  return {};
}

void QuotaCharge::shrink(const std::size_t bytes) noexcept {
  if (Quota* const quota = m_quota.get()) {
    [[maybe_unused]] const std::size_t had = m_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    KASSERT(had >= bytes);
    quota->uncharge(bytes);
  }
}

void QuotaCharge::reset() noexcept {
  if (Quota* const quota = m_quota.get()) {
    quota->uncharge(m_bytes.exchange(0, std::memory_order_relaxed));
    m_quota.reset();
  }
}
}  // namespace kernel::memory
