/// @file driver_caps.hpp
/// @brief What a driver interacts with hardware through: its interrupt, its registers, memory a
/// device can reach, and memory it shares with others.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include "core/cap.hpp"
#include "core/errors.hpp"
#include "core/ipc/notification.hpp"
#include "hal/apic/ioapic.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "memory/address.hpp"
#include "memory/vmm/vma/vm_object.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/lock.hpp"

namespace kernel::core::ipc {
/// @brief A device interrupt, delivered as a signal on a notification.
///
/// A level-triggered I/O APIC pin is masked as the interrupt comes in, as it would fire again for
/// as long as the device holds the line, and acknowledge() unmasks it once the driver has dealt
/// with the device.
class alignas(std::hardware_destructive_interference_size) IrqHandler final
    : public TypedCapObject<IrqHandler, CapType::IrqHandler> {
 public:
  static constexpr std::uint8_t FIRST_VECTOR = 32;
  static constexpr std::uint8_t LAST_VECTOR = 0xEF;

  IrqHandler(std::uint8_t vector, std::optional<std::uint32_t> gsi, bool level_triggered) noexcept;

  /// @brief Unregisters, which waits for a handler that is running, and masks the pin.
  ~IrqHandler() noexcept;

  /// @brief A handler for interrupts on @p vector, with no pin behind it (MSI, or IPIs in tests).
  [[nodiscard]] static std::expected<CapRef<IrqHandler>, Error> create_vector(
      std::uint8_t vector) noexcept;

  /// @brief A handler for I/O APIC pin @p gsi, routed to @p vector on @p target_apic_id, masked
  ///        until a notification is set.
  [[nodiscard]] static std::expected<CapRef<IrqHandler>, Error> create_gsi(
      std::uint32_t gsi, std::uint8_t vector, std::uint8_t target_apic_id,
      hw::apic::PinPolarity polarity = hw::apic::PinPolarity::High,
      hw::apic::TriggerMode trigger = hw::apic::TriggerMode::Edge) noexcept;

  /// @brief Signals @p badge on @p notification for every interrupt from now on. Takes Manage.
  [[nodiscard]] std::expected<void, Error> set_notification(CapRights rights,
                                                            Notification& notification,
                                                            std::uint64_t badge) noexcept;

  /// @brief Stops signalling, and masks the pin. Takes Manage.
  [[nodiscard]] std::expected<void, Error> clear_notification(CapRights rights) noexcept;

  /// @brief Unmasks a level-triggered pin the last interrupt masked. Takes Manage.
  [[nodiscard]] std::expected<void, Error> acknowledge(CapRights rights) noexcept;

  [[nodiscard]] constexpr std::uint8_t vector() const noexcept { return m_vector; }
  [[nodiscard]] constexpr std::optional<std::uint32_t> gsi() const noexcept { return m_gsi; }
  [[nodiscard]] constexpr bool is_level_triggered() const noexcept { return m_level_triggered; }

  [[nodiscard]] bool is_masked() const noexcept { return m_masked.load(std::memory_order_acquire); }

  [[nodiscard]] std::uint64_t irq_count() const noexcept {
    return m_count.load(std::memory_order_relaxed);
  }

 private:
  static hw::interrupts::InterruptStatus on_interrupt(const hw::interrupts::Event& event,
                                                      void* self) noexcept;

  std::uint8_t m_vector;
  std::optional<std::uint32_t> m_gsi;
  bool m_level_triggered;
  std::atomic<bool> m_masked{false};
  std::atomic<std::uint64_t> m_count{0};

  utils::QSpinlock m_lock{};
  Notification* m_notification{nullptr};  ///< With a reference.
  std::uint64_t m_badge{0};

  hw::interrupts::InterruptRegistration m_registration;
  bool m_registered{false};

  // Hands interrupts on the vector to this, from now until it is destroyed.
  void register_handler() noexcept;
};

/// @brief A device's registers: a page-aligned physical range with no RAM in it.
class alignas(16) MmioRegion final : public TypedCapObject<MmioRegion, CapType::Mmio> {
 public:
  constexpr MmioRegion(const memory::PhysicalAddress base, const std::size_t size,
                       const memory::vmm::CacheMode cache) noexcept
      : m_base(base), m_size(size), m_cache(cache) {}

  /// @return InvalidAlignment for a range that is not page-aligned, SecurityViolation for one with
  ///         RAM in it, OutOfBounds for one that wraps around.
  [[nodiscard]] static std::expected<CapRef<MmioRegion>, Error> create(
      memory::PhysicalAddress base, std::size_t size,
      memory::vmm::CacheMode cache = memory::vmm::CacheMode::Uncacheable) noexcept;

  /// @brief A region for part of this one, @p size bytes from @p offset, counted against
  ///        @p quota if there is one. Takes Mint.
  [[nodiscard]] std::expected<CapRef<MmioRegion>, Error> derive(
      CapRights rights, std::size_t offset, std::size_t size, memory::vmm::CacheMode cache,
      memory::Quota* quota = nullptr) const noexcept;

  /// @brief Maps @p length bytes from @p offset (0: to the end) into @p space. Takes Read; Write
  ///        makes it writable.
  [[nodiscard]] std::expected<memory::VirtualAddress, Error> map_into(
      memory::vmm::AddressSpace& space, CapRights rights, std::size_t offset = 0,
      std::size_t length = 0, bool user_accessible = true,
      memory::VirtualAddress hint = memory::VirtualAddress{}) const noexcept;

  [[nodiscard]] constexpr memory::PhysicalAddress base() const noexcept { return m_base; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return m_size; }
  [[nodiscard]] constexpr memory::vmm::CacheMode cache() const noexcept { return m_cache; }

 private:
  memory::PhysicalAddress m_base;
  std::size_t m_size;
  memory::vmm::CacheMode m_cache;
};

/// @brief Where one page of a DMA buffer is, for the device.
struct DmaSegment {
  memory::PhysicalAddress phys_addr{};
  std::size_t size_bytes{0};
};

/// @brief A DMA buffer as allocate() made it.
struct DmaBufferAllocation {
  memory::VirtualAddress virt_addr{};
  memory::PhysicalAddress first_phys_addr{};
  std::size_t size_bytes{0};
};

/// @brief A quota of memory that devices may read and write, in buffers mapped into a driver.
///
/// Each buffer is a VM object of its own, filled at once, whose pages never move or go while it
/// lives: the pool holds one reference, and the mapping another. Freeing a buffer unmaps it and
/// gives the quota back; the pool going lets go of whatever buffers are left, whose pages then go
/// with their mappings.
class alignas(16) DmaPool final : public TypedCapObject<DmaPool, CapType::DmaPool> {
 public:
  explicit DmaPool(const std::size_t quota) noexcept : m_quota(quota) {}
  ~DmaPool() noexcept;

  [[nodiscard]] static std::expected<CapRef<DmaPool>, Error> create(std::size_t quota) noexcept;

  /// @brief A buffer of @p size bytes, mapped into @p space. Takes Read, Write and Manage.
  /// @param segments Where each page is, as many as fit.
  /// @return OutOfMemory past the quota.
  [[nodiscard]] std::expected<DmaBufferAllocation, Error> allocate(
      memory::vmm::AddressSpace& space, CapRights rights, std::size_t size,
      bool user_accessible = true, memory::vmm::CacheMode cache = memory::vmm::CacheMode::WriteBack,
      std::span<DmaSegment> segments = {}) noexcept;

  /// @brief Frees the buffer allocate() mapped at @p virt in @p space. Takes Manage.
  /// @return NotFound if this pool made no buffer there.
  [[nodiscard]] std::expected<void, Error> free(memory::vmm::AddressSpace& space, CapRights rights,
                                                memory::VirtualAddress virt) noexcept;

  [[nodiscard]] constexpr std::size_t quota() const noexcept { return m_quota; }

  [[nodiscard]] std::size_t allocated() const noexcept {
    return m_allocated.load(std::memory_order_acquire);
  }

 private:
  struct Buffer {
    Buffer* next;
    memory::vmm::VMObject* object;  ///< With the pool's reference.
    memory::vmm::AddressSpace* space;
    memory::VirtualAddress virt;
    std::size_t size;
  };

  std::size_t m_quota;
  std::atomic<std::size_t> m_allocated{0};
  utils::QSpinlock m_lock{};
  Buffer* m_buffers{nullptr};
};

/// @brief Memory to share: a window onto a VM object, which several address spaces can map.
///
/// A capability to the whole object can make one to a part of it (derive()), which never reaches
/// past that part, however it is used, and cannot grow.
class alignas(16) SharedVmObject final : public TypedCapObject<SharedVmObject, CapType::VmObject> {
 public:
  /// @pre @p object comes with a reference for this to keep.
  SharedVmObject(memory::vmm::VMObject* object, std::size_t first_page, std::size_t pages,
                 bool is_slice) noexcept;
  ~SharedVmObject() noexcept;

  /// @brief A new object of @p size bytes, rounded up to whole pages.
  /// @param quota What it counts against, if anything: itself and all of its pages, for as long
  ///              as anything maps them or has a window onto them.
  /// @return QuotaExceeded if the quota has no room for that.
  [[nodiscard]] static std::expected<CapRef<SharedVmObject>, Error> create(
      std::size_t size, memory::vmm::PageSize page_size = memory::vmm::PageSize::Size4K,
      memory::Quota* quota = nullptr) noexcept;

  /// @brief A window onto @p pages pages of @p object from @p first_page, which takes a reference
  ///        of its own.
  [[nodiscard]] static std::expected<CapRef<SharedVmObject>, Error> wrap(
      memory::vmm::VMObject& object, std::size_t first_page, std::size_t pages) noexcept;

  /// @brief A window onto @p pages pages of this one, from @p first_page, counted against
  ///        @p quota if there is one. The pages count against what they did already. Takes Mint.
  [[nodiscard]] std::expected<CapRef<SharedVmObject>, Error> derive(
      CapRights rights, std::size_t first_page, std::size_t pages,
      memory::Quota* quota = nullptr) const noexcept;

  /// @brief Makes the window @p pages long. Only grows, and not a part of another. The pages added
  ///        count against the quota the object's pages do. Takes Write and Manage.
  /// @return QuotaExceeded if there is no room for them.
  [[nodiscard]] std::expected<void, Error> resize(CapRights rights, std::size_t pages) noexcept;

  /// @brief Maps @p pages pages from @p first_page (0 pages: to the end) into @p space, with
  ///        @p access, which @p rights must allow: Read, Write, Execute.
  [[nodiscard]] std::expected<memory::VirtualAddress, Error> map_into(
      memory::vmm::AddressSpace& space, CapRights rights, memory::vmm::AccessFlags access,
      std::size_t first_page = 0, std::size_t pages = 0,
      memory::VirtualAddress hint = memory::VirtualAddress{}) const noexcept;

  /// @brief Where page @p page of the window is, filling it in first if need be. Takes Read.
  [[nodiscard]] std::expected<memory::PhysicalAddress, Error> translate(
      CapRights rights, std::size_t page) const noexcept;

  [[nodiscard]] constexpr memory::vmm::VMObject* vm_object() const noexcept { return m_object; }
  [[nodiscard]] constexpr std::size_t first_page() const noexcept { return m_first_page; }
  [[nodiscard]] std::size_t pages() const noexcept {
    return m_pages.load(std::memory_order_acquire);
  }
  [[nodiscard]] constexpr bool is_slice() const noexcept { return m_is_slice; }

 private:
  memory::vmm::VMObject* m_object;
  std::size_t m_first_page;
  std::atomic<std::size_t> m_pages;
  bool m_is_slice;
};
}  // namespace kernel::core::ipc
