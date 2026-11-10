#include "core/ipc/driver_caps.hpp"

#include "boot/boot.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "utils/maths.hpp"

namespace kernel::core::ipc {
namespace {
using Guard = utils::IrqSaveGuard<utils::QSpinlock>;

// Whether [@p base, @p base + @p size) has any memory the kernel keeps track of in it: RAM, the
// kernel itself, ACPI tables. Device registers and firmware's reserved ranges are fine.
[[nodiscard]] bool overlaps_ram(memory::PhysicalAddress base, std::size_t size) noexcept {
  const limine_memmap_response* memmap = boot::memmap_request.response;
  if (memmap == nullptr) [[unlikely]] {
    return true;
  }

  const std::uint64_t start = base.value();
  const std::uint64_t end = start + size;
  for (std::uint64_t i = 0; i < memmap->entry_count; ++i) {
    const limine_memmap_entry& entry = *memmap->entries[i];
    switch (entry.type) {
      case LIMINE_MEMMAP_USABLE:
      case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
      case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
      case LIMINE_MEMMAP_ACPI_NVS:
      case LIMINE_MEMMAP_BAD_MEMORY:
      case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        if (start < entry.base + entry.length && entry.base < end) {
          return true;
        }
        break;
      default:
        break;
    }
  }

  return false;
}

// @p count pages of @p page_bytes each, without overflowing. @return Nothing if it would.
[[nodiscard]] std::optional<std::size_t> pages_to_bytes(std::size_t count,
                                                        std::size_t page_bytes) noexcept {
  std::size_t bytes = 0;
  if (__builtin_mul_overflow(count, page_bytes, &bytes)) [[unlikely]] {
    return std::nullopt;
  }

  return bytes;
}
}  // namespace

IrqHandler::IrqHandler(std::uint8_t vector, std::optional<std::uint32_t> gsi,
                       bool level_triggered) noexcept
    : m_vector(vector),
      m_gsi(gsi),
      m_level_triggered(level_triggered),
      m_registration(&IrqHandler::on_interrupt, this) {}

IrqHandler::~IrqHandler() noexcept {
  if (m_gsi) {
    (void)hw::apic::io_apic_manager.mask_gsi(*m_gsi);
  }

  if (m_registered) {
    hw::interrupts::InterruptManager::unregister_handler(m_vector, &m_registration);
  }

  if (Notification* notification = std::exchange(m_notification, nullptr)) {
    notification->release_ref();
  }
}

hw::interrupts::InterruptStatus IrqHandler::on_interrupt(const hw::interrupts::Event&,
                                                         void* const self) noexcept {
  auto* handler = static_cast<IrqHandler*>(self);
  handler->m_count.fetch_add(1, std::memory_order_relaxed);

  if (handler->m_gsi && handler->m_level_triggered) {
    handler->m_masked.store(true, std::memory_order_release);
    (void)hw::apic::io_apic_manager.mask_gsi(*handler->m_gsi);
  }

  Guard guard{handler->m_lock};
  if (handler->m_notification != nullptr) {
    handler->m_notification->signal(handler->m_badge);
  }

  return hw::interrupts::InterruptStatus::Handled;
}

void IrqHandler::register_handler() noexcept {
  m_registered = true;
  hw::interrupts::InterruptManager::register_handler(m_vector, &m_registration);
}

std::expected<CapRef<IrqHandler>, Error> IrqHandler::create_vector(std::uint8_t vector) noexcept {
  if (vector < FIRST_VECTOR || vector > LAST_VECTOR) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  auto handler = create(vector, std::nullopt, false);
  if (handler) [[likely]] {
    (*handler)->register_handler();
  }

  return handler;
}

std::expected<CapRef<IrqHandler>, Error> IrqHandler::create_gsi(
    std::uint32_t gsi, std::uint8_t vector, std::uint8_t target_apic_id,
    hw::apic::PinPolarity polarity, hw::apic::TriggerMode trigger) noexcept {
  if (vector < FIRST_VECTOR || vector > LAST_VECTOR) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const auto routed = hw::apic::io_apic_manager.route_direct_gsi(gsi, vector, target_apic_id,
                                                                 polarity, trigger, true);
  if (!routed) [[unlikely]] {
    return std::unexpected(routed.error());
  }

  auto handler = create(vector, gsi, trigger == hw::apic::TriggerMode::Level);
  if (!handler) [[unlikely]] {
    return handler;
  }

  (*handler)->m_masked.store(true, std::memory_order_relaxed);
  (*handler)->register_handler();
  return handler;
}

std::expected<void, Error> IrqHandler::set_notification(CapRights rights,
                                                        Notification& notification,
                                                        std::uint64_t badge) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (badge == 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  notification.add_ref();
  Notification* previous;
  {
    Guard guard{m_lock};
    previous = std::exchange(m_notification, &notification);
    m_badge = badge;
  }

  if (previous != nullptr) {
    previous->release_ref();
  }

  if (m_gsi && m_masked.exchange(false, std::memory_order_acq_rel)) {
    return hw::apic::io_apic_manager.unmask_gsi(*m_gsi);
  }

  return {};
}

std::expected<void, Error> IrqHandler::clear_notification(CapRights rights) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (m_gsi) {
    m_masked.store(true, std::memory_order_release);
    (void)hw::apic::io_apic_manager.mask_gsi(*m_gsi);
  }

  Notification* previous;
  {
    Guard guard{m_lock};
    previous = std::exchange(m_notification, nullptr);
    m_badge = 0;
  }

  if (previous != nullptr) {
    previous->release_ref();
  }

  return {};
}

std::expected<void, Error> IrqHandler::acknowledge(CapRights rights) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (m_gsi && m_level_triggered && m_masked.exchange(false, std::memory_order_acq_rel)) {
    return hw::apic::io_apic_manager.unmask_gsi(*m_gsi);
  }

  return {};
}

std::expected<CapRef<MmioRegion>, Error> MmioRegion::create(memory::PhysicalAddress base,
                                                            std::size_t size,
                                                            memory::vmm::CacheMode cache) noexcept {
  if (base.is_null() || size == 0 || !utils::maths::is_aligned(base.value(), memory::PAGE_SIZE) ||
      !utils::maths::is_aligned(size, memory::PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  std::uint64_t end = 0;
  if (__builtin_add_overflow(base.value(), size, &end)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  // Mapping RAM this way would hand out the kernel's own memory.
  if (overlaps_ram(base, size)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  return TypedCapObject::create(base, size, cache);
}

std::expected<CapRef<MmioRegion>, Error> MmioRegion::derive(
    CapRights rights, std::size_t offset, std::size_t size,
    memory::vmm::CacheMode cache) const noexcept {
  if (!has_rights(rights, CapRights::Mint)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (size == 0 || offset >= m_size || size > m_size - offset) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  if (!utils::maths::is_aligned(offset, memory::PAGE_SIZE) ||
      !utils::maths::is_aligned(size, memory::PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  return TypedCapObject::create(m_base + offset, size, cache);
}

std::expected<memory::VirtualAddress, Error> MmioRegion::map_into(
    memory::vmm::AddressSpace& space, CapRights rights, std::size_t offset, std::size_t length,
    bool user_accessible, memory::VirtualAddress hint) const noexcept {
  if (!has_rights(rights, CapRights::Read)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (offset >= m_size) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  const std::size_t bytes = length == 0 ? m_size - offset : length;
  if (bytes > m_size - offset) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  if (!utils::maths::is_aligned(offset, memory::PAGE_SIZE) ||
      !utils::maths::is_aligned(bytes, memory::PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  using memory::vmm::AccessFlags;
  AccessFlags access = AccessFlags::Read | AccessFlags::Mmio;
  if (has_rights(rights, CapRights::Write)) {
    access = access | AccessFlags::Write;
  }

  if (user_accessible) {
    access = access | AccessFlags::User;
  }

  return space.map_mmio(m_base + offset, bytes, access, m_cache, memory::vmm::PageSize::Size4K, 0,
                        hint);
}

DmaPool::~DmaPool() noexcept {
  while (Buffer* buffer = m_buffers) {
    m_buffers = buffer->next;
    buffer->object->drop_ref();
    memory::heap::kfree(buffer);
  }
}

std::expected<CapRef<DmaPool>, Error> DmaPool::create(std::size_t quota) noexcept {
  if (quota == 0 || quota > ~std::size_t{0} - memory::PAGE_SIZE) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  return TypedCapObject::create(utils::maths::align_up(quota, memory::PAGE_SIZE));
}

std::expected<DmaBufferAllocation, Error> DmaPool::allocate(
    memory::vmm::AddressSpace& space, CapRights rights, std::size_t size, bool user_accessible,
    memory::vmm::CacheMode cache, std::span<DmaSegment> segments) noexcept {
  if (!has_rights(rights, CapRights::ReadWrite | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (size == 0 || size > m_quota) [[unlikely]] {
    return std::unexpected(size == 0 ? Error::InvalidArgument : Error::OutOfMemory);
  }

  const std::size_t bytes = utils::maths::align_up(size, memory::PAGE_SIZE);
  const std::size_t pages = bytes / memory::PAGE_SIZE;

  // The quota first, so that two allocations at once cannot both fit in what is left for one.
  std::size_t taken = m_allocated.load(std::memory_order_relaxed);
  do {
    if (bytes > m_quota - taken) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }
  } while (!m_allocated.compare_exchange_weak(taken, taken + bytes, std::memory_order_acq_rel,
                                              std::memory_order_relaxed));

  const auto give_back = [&] { m_allocated.fetch_sub(bytes, std::memory_order_acq_rel); };

  auto* buffer = static_cast<Buffer*>(memory::heap::kmalloc(sizeof(Buffer), alignof(Buffer)));
  memory::vmm::VMObject* object =
      buffer != nullptr ? memory::vmm::VMObject::create(memory::vmm::VMAType::SharedIPC,
                                                        memory::vmm::PageSize::Size4K, pages)
                        : nullptr;
  if (object == nullptr) [[unlikely]] {
    memory::heap::kfree(buffer);
    give_back();
    return std::unexpected(Error::OutOfMemory);
  }

  // Every page now, so that each has its address before the device is told about it.
  memory::PhysicalAddress first{};
  for (std::size_t i = 0; i < pages; ++i) {
    const auto page = object->get_page(i);
    if (!page) [[unlikely]] {
      object->drop_ref();
      memory::heap::kfree(buffer);
      give_back();
      return std::unexpected(page.error());
    }

    if (i == 0) {
      first = *page;
    }

    if (i < segments.size()) {
      segments[i] = DmaSegment{.phys_addr = *page, .size_bytes = memory::PAGE_SIZE};
    }
  }

  using memory::vmm::AccessFlags;
  AccessFlags access = AccessFlags::Read | AccessFlags::Write;
  if (user_accessible) {
    access = access | AccessFlags::User;
  }

  const auto mapped =
      space.map_object(object, bytes, 0, access, cache, memory::vmm::PageSize::Size4K,
                       memory::vmm::VMAType::SharedIPC);
  if (!mapped) [[unlikely]] {
    object->drop_ref();
    memory::heap::kfree(buffer);
    give_back();
    return std::unexpected(mapped.error());
  }

  *buffer =
      Buffer{.next = nullptr, .object = object, .space = &space, .virt = *mapped, .size = bytes};
  {
    Guard guard{m_lock};
    buffer->next = m_buffers;
    m_buffers = buffer;
  }

  return DmaBufferAllocation{.virt_addr = *mapped, .first_phys_addr = first, .size_bytes = bytes};
}

std::expected<void, Error> DmaPool::free(memory::vmm::AddressSpace& space, CapRights rights,
                                         memory::VirtualAddress virt) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  Buffer* buffer = nullptr;
  {
    Guard guard{m_lock};
    for (Buffer** link = &m_buffers; *link != nullptr; link = &(*link)->next) {
      if ((*link)->space == &space && (*link)->virt == virt) {
        buffer = *link;
        *link = buffer->next;
        break;
      }
    }
  }

  // Only a buffer of this pool, so a capability to it cannot unmap anything else.
  if (buffer == nullptr) [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  (void)space.free(virt);
  buffer->object->drop_ref();
  m_allocated.fetch_sub(buffer->size, std::memory_order_acq_rel);
  memory::heap::kfree(buffer);
  return {};
}

SharedVmObject::SharedVmObject(memory::vmm::VMObject* const object, std::size_t first_page,
                               std::size_t pages, bool is_slice) noexcept
    : m_object(object), m_first_page(first_page), m_pages(pages), m_is_slice(is_slice) {}

SharedVmObject::~SharedVmObject() noexcept { m_object->drop_ref(); }

std::expected<CapRef<SharedVmObject>, Error> SharedVmObject::create(
    std::size_t size, memory::vmm::PageSize page_size) noexcept {
  const std::size_t page_bytes = memory::vmm::get_page_bytes(page_size);
  if (size == 0 || size > ~std::size_t{0} - page_bytes) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const std::size_t pages = utils::maths::align_up(size, page_bytes) / page_bytes;
  memory::vmm::VMObject* object =
      memory::vmm::VMObject::create(memory::vmm::VMAType::SharedIPC, page_size, pages);
  if (object == nullptr) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  auto shared = TypedCapObject::create(object, 0, pages, false);
  if (!shared) [[unlikely]] {
    object->drop_ref();
  }

  return shared;
}

std::expected<CapRef<SharedVmObject>, Error> SharedVmObject::wrap(memory::vmm::VMObject& object,
                                                                  std::size_t first_page,
                                                                  std::size_t pages) noexcept {
  if (pages == 0 || first_page > ~std::size_t{0} - pages) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  object.add_ref();
  auto shared = TypedCapObject::create(&object, first_page, pages, false);
  if (!shared) [[unlikely]] {
    object.drop_ref();
  }

  return shared;
}

std::expected<CapRef<SharedVmObject>, Error> SharedVmObject::derive(
    CapRights rights, std::size_t first_page, std::size_t pages) const noexcept {
  if (!has_rights(rights, CapRights::Mint)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const std::size_t window = this->pages();
  if (pages == 0 || first_page >= window || pages > window - first_page) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  m_object->add_ref();
  auto slice = TypedCapObject::create(m_object, m_first_page + first_page, pages, true);
  if (!slice) [[unlikely]] {
    m_object->drop_ref();
  }

  return slice;
}

std::expected<void, Error> SharedVmObject::resize(CapRights rights, std::size_t pages) noexcept {
  if (!has_rights(rights, CapRights::Write | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (m_is_slice) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  if (pages <= this->pages() || m_first_page > ~std::size_t{0} - pages) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  m_object->expand_max_pages(m_first_page + pages);
  m_pages.store(pages, std::memory_order_release);
  return {};
}

std::expected<memory::VirtualAddress, Error> SharedVmObject::map_into(
    memory::vmm::AddressSpace& space, CapRights rights, memory::vmm::AccessFlags access,
    std::size_t first_page, std::size_t pages, memory::VirtualAddress hint) const noexcept {
  using memory::vmm::AccessFlags;
  using memory::vmm::has_flag;

  if ((has_flag(access, AccessFlags::Read) && !has_rights(rights, CapRights::Read)) ||
      (has_flag(access, AccessFlags::Write) && !has_rights(rights, CapRights::Write)) ||
      (has_flag(access, AccessFlags::Execute) && !has_rights(rights, CapRights::Execute)))
      [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const std::size_t window = this->pages();
  if (first_page >= window) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  const std::size_t count = pages == 0 ? window - first_page : pages;
  if (count > window - first_page) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  const std::size_t page_bytes = memory::vmm::get_page_bytes(m_object->page_size());
  const auto bytes = pages_to_bytes(count, page_bytes);
  if (!bytes) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  return space.map_object(m_object, *bytes, m_first_page + first_page, access,
                          memory::vmm::CacheMode::WriteBack, m_object->page_size(),
                          memory::vmm::VMAType::SharedIPC, 0, hint);
}

std::expected<memory::PhysicalAddress, Error> SharedVmObject::translate(
    CapRights rights, std::size_t page) const noexcept {
  if (!has_rights(rights, CapRights::Read)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (page >= pages()) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  return m_object->get_page(m_first_page + page);
}
}  // namespace kernel::core::ipc
