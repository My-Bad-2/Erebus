#pragma once

#include "interrupts.hpp"
#include "utils/linked_list.hpp"
#include "utils/lock.hpp"
#include <array>
#include <cstdint>

namespace kernel::hw::interrupts {
enum class InterruptStatus : std::uint8_t {
  Handled,
  Unhandled,
  YieldRequested,
  ScheduleDPC, // ISR requests Bottom-Half deferral
};

using Callback = InterruptStatus (*)(const Event &, void *ctx);
using DpcCallback = void (*)(void *context);

class InterruptRegistration : public utils::SListNode<InterruptRegistration> {
  friend class InterruptManager;

public:
  constexpr InterruptRegistration(const Callback cb, void *ctx, const DpcCallback dpc = nullptr) noexcept
      : m_callback(cb), m_ctx(ctx), m_dpc(dpc) {}

  InterruptRegistration(const InterruptRegistration &) = delete;
  InterruptRegistration &operator=(const InterruptRegistration &) = delete;

private:
  Callback m_callback{nullptr};
  void *m_ctx{nullptr};
  DpcCallback m_dpc{nullptr};
};

class InterruptManager {
  struct alignas(std::hardware_destructive_interference_size) VectorSlot {
    utils::QSpinlock lock;

    Callback direct_callback;
    DpcCallback direct_dpc;
    void *direct_ctx;

    constexpr VectorSlot() noexcept : direct_callback(nullptr), direct_dpc(nullptr), direct_ctx(nullptr) {}

    utils::SList<InterruptRegistration> list;
  };

  static inline std::array<VectorSlot, IDT_SIZE> s_slots{};

  static void handle_exception(const Event &event) noexcept;
  static void handle_nmi(const Event &event) noexcept;

public:
  static void initialize() noexcept {
    for (auto &slot : s_slots) {
      slot.list.clear();
    }
  }

  static void register_handler(std::uint8_t vector, InterruptRegistration *reg) noexcept;
  static void unregister_handler(std::uint8_t vector, InterruptRegistration *reg) noexcept;
  static void route_event(const Event &event) noexcept;
};
} // namespace kernel::hw::interrupts