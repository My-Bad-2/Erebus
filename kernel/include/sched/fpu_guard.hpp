/// @file fpu_guard.hpp
/// @brief Using the FPU from kernel code.
///
/// The kernel is compiled to stay off the FPU, SSE and AVX registers, so inline assembly that
/// wants them has to borrow them from the running thread, which is what the guard does:
///
/// @code
/// utils::QSpinlock lock;  // whatever protects the data the section works on
///
/// {
///   utils::FpuGuard guard{lock};
///   asm volatile("movq %%rax, %%xmm0" ::: "memory");
/// }
/// @endcode
///
/// Sections can nest. Only thread context can use them: see sched::Thread::begin_kernel_fpu().
#pragma once

#include "sched/thread.hpp"
#include "utils/locks/guards.hpp"

namespace kernel::utils {
/// @brief Holds @p LockType and keeps the thread on this CPU, with its FPU registers free to use.
///
/// A kernel thread's registers are its own, so what one section leaves in them is there for the
/// next one, on this CPU or any other. A user thread gets its registers back the way they were.
template <BasicLockable LockType>
class [[nodiscard]] FpuGuard {
  // In this order: the first FPU use of a thread allocates its save area, which takes its time
  // and must not happen with the lock held.
  sched::Thread* m_thread{sched::current()};
  bool m_context_ready;
  PreemptGuard<LockType> m_preempt_guard;
  sched::KernelFpuSection m_section{sched::KernelFpuSection::Nested};

  [[nodiscard]] bool prepare_context(const bool amx) const noexcept {
    // Before the CPU has a thread there is nobody whose registers these could be.
    if (m_thread == nullptr) {
      return !amx;
    }

    return (amx ? m_thread->request_amx_context() : m_thread->enable_fpu_context()).has_value();
  }

 public:
  /// @param lock        Taken for as long as the guard lives.
  /// @param require_amx Whether the section wants the AMX tile registers.
  explicit FpuGuard(LockType& lock, const bool require_amx = false) noexcept
      : m_context_ready(prepare_context(require_amx)), m_preempt_guard{lock} {
    if (m_thread != nullptr) {
      m_section = m_thread->begin_kernel_fpu();
    }
  }

  ~FpuGuard() noexcept {
    if (m_thread != nullptr) {
      m_thread->end_kernel_fpu(m_section);
    }
  }

  /// @brief Whether the thread got what the guard asked for. If not, nothing stops plain FPU use:
  ///        it just is not saved across a switch. With @p require_amx the tile registers remain
  ///        off limits, and using them would fault.
  [[nodiscard]] bool context_ready() const noexcept { return m_context_ready; }

  FpuGuard(const FpuGuard&) = delete;
  FpuGuard& operator=(const FpuGuard&) = delete;
};
}  // namespace kernel::utils
