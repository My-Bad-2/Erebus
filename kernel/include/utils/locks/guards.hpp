#pragma once

#include "hal/hal.hpp"
#include "hal/percpu.hpp"

namespace kernel::utils {
template <typename T>
concept BasicLockable = requires(T a) {
  { a.lock() } noexcept -> std::same_as<void>;
  { a.unlock() } noexcept -> std::same_as<void>;
};

template <typename T>
concept SharedLockable = requires(T a) {
  { a.read_lock() } noexcept -> std::same_as<void>;
  { a.read_unlock() } noexcept -> std::same_as<void>;
};

template <typename T>
concept ExclusiveLockable = requires(T a) {
  { a.write_lock() } noexcept -> std::same_as<void>;
  { a.write_unlock() } noexcept -> std::same_as<void>;
};

template <BasicLockable LockType> class [[nodiscard]] NakedGuard {
  LockType &m_lock;

public:
  explicit NakedGuard(LockType &lock) noexcept : m_lock(lock) { m_lock.lock(); }

  ~NakedGuard() noexcept { m_lock.unlock(); }

  NakedGuard(const NakedGuard &) = delete;
  NakedGuard &operator=(const NakedGuard &) = delete;
};

template <BasicLockable LockType> class [[nodiscard]] PreemptGuard {
  LockType &m_lock;

public:
  explicit PreemptGuard(LockType &lock) noexcept : m_lock(lock) {
    hw::percpu::preempt_disable();
    m_lock.lock();
  }

  ~PreemptGuard() noexcept {
    m_lock.unlock();
    hw::percpu::preempt_enable();
  }

  PreemptGuard(const PreemptGuard &) = delete;
  PreemptGuard &operator=(const PreemptGuard &) = delete;
};

template <BasicLockable LockType> class [[nodiscard]] IrqSaveGuard {
  LockType &m_lock;
  std::uint64_t m_saved_flags;

public:
  explicit IrqSaveGuard(LockType &lock) noexcept : m_lock(lock) {
    m_saved_flags = hw::irq::read_flags();
    hw::irq::disable();

    hw::percpu::preempt_disable();
    m_lock.lock();
  }

  ~IrqSaveGuard() noexcept {
    m_lock.unlock();

    hw::percpu::preempt_enable();
    hw::irq::write_flags(m_saved_flags);
  }

  IrqSaveGuard(const IrqSaveGuard &) = delete;
  IrqSaveGuard &operator=(const IrqSaveGuard &) = delete;
};

template <SharedLockable LockType> class [[nodiscard]] SharedPreemptGuard {
  LockType &m_lock;

public:
  explicit SharedPreemptGuard(LockType &lock) noexcept : m_lock(lock) {
    hw::percpu::preempt_disable();
    m_lock.read_lock();
  }

  ~SharedPreemptGuard() noexcept {
    m_lock.read_unlock();
    hw::percpu::preempt_enable();
  }

  SharedPreemptGuard(const SharedPreemptGuard &) = delete;
  SharedPreemptGuard &operator=(const SharedPreemptGuard &) = delete;
};

template <SharedLockable LockType> class [[nodiscard]] SharedIrqSaveGuard {
  LockType &m_lock;
  std::uint64_t m_saved_flags;

public:
  explicit SharedIrqSaveGuard(LockType &lock) noexcept : m_lock(lock) {
    m_saved_flags = hw::irq::read_flags();
    hw::irq::disable();

    hw::percpu::preempt_disable();
    m_lock.read_lock();
  }

  ~SharedIrqSaveGuard() noexcept {
    m_lock.read_unlock();
    hw::percpu::preempt_enable();
    hw::irq::write_flags(m_saved_flags);
  }

  SharedIrqSaveGuard(const SharedIrqSaveGuard &) = delete;
  SharedIrqSaveGuard &operator=(const SharedIrqSaveGuard &) = delete;
};

template <ExclusiveLockable LockType> class [[nodiscard]] ExclusiveNakedGuard {
  LockType &m_lock;

public:
  explicit ExclusiveNakedGuard(LockType &lock) noexcept : m_lock(lock) { m_lock.write_lock(); }

  ~ExclusiveNakedGuard() noexcept { m_lock.write_unlock(); }

  ExclusiveNakedGuard(const ExclusiveNakedGuard &) = delete;
  ExclusiveNakedGuard &operator=(const ExclusiveNakedGuard &) = delete;
};

template <ExclusiveLockable LockType> class [[nodiscard]] ExclusivePreemptGuard {
  LockType &m_lock;

public:
  explicit ExclusivePreemptGuard(LockType &lock) noexcept : m_lock(lock) {
    hw::percpu::preempt_disable();
    m_lock.write_lock();
  }

  ~ExclusivePreemptGuard() noexcept {
    m_lock.write_unlock();
    hw::percpu::preempt_enable();
  }

  ExclusivePreemptGuard(const ExclusivePreemptGuard &) = delete;
  ExclusivePreemptGuard &operator=(const ExclusivePreemptGuard &) = delete;
};

template <ExclusiveLockable LockType> class [[nodiscard]] ExclusiveIrqSaveGuard {
  LockType &m_lock;
  std::uint64_t m_saved_flags;

public:
  explicit ExclusiveIrqSaveGuard(LockType &lock) noexcept : m_lock(lock) {
    m_saved_flags = hw::irq::read_flags();
    hw::irq::disable();

    hw::percpu::preempt_disable();
    m_lock.write_lock();
  }

  ~ExclusiveIrqSaveGuard() noexcept {
    m_lock.write_unlock();
    hw::percpu::preempt_enable();
    hw::irq::write_flags(m_saved_flags);
  }

  ExclusiveIrqSaveGuard(const ExclusiveIrqSaveGuard &) = delete;
  ExclusiveIrqSaveGuard &operator=(const ExclusiveIrqSaveGuard &) = delete;
};
} // namespace kernel::utils