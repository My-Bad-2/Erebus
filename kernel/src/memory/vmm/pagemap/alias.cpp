#include "memory/vmm/pagemap/pagemap.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> PageMap::alias_virtual_range(VirtualAddress src_virt, VirtualAddress dest_virt,
                                                        std::size_t size_bytes) noexcept {
  if (!src_virt.is_aligned(PAGE_SIZE) || !dest_virt.is_aligned(PAGE_SIZE) ||
      !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  const std::uintptr_t src_end = src_virt.value() + size_bytes;
  const std::uintptr_t dst_end = dest_virt.value() + size_bytes;
  if (std::max(src_virt.value(), dest_virt.value()) < std::min(src_end, dst_end)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  VirtualAddress curr_src = src_virt;
  VirtualAddress curr_dest = dest_virt;
  std::size_t remaining = size_bytes;

  while (remaining > 0) {
    auto trans = translate(curr_src);
    if (!trans) [[unlikely]] {
      [[maybe_unused]] auto _ = unmap_range(dest_virt, size_bytes - remaining);
      return std::unexpected(Error::NotMapped);
    }

    const std::size_t page_bytes = size_to_bytes(trans->mapped_size);
    const std::size_t offset_in_page = curr_src.page_offset(page_bytes);

    const std::size_t available_in_page = page_bytes - offset_in_page;
    const std::size_t chunk_bytes = std::min(remaining, available_in_page);
    const PhysicalAddress target_phys = trans->phys_address + offset_in_page;

    const auto map_res = map_range(curr_dest, target_phys, chunk_bytes, trans->flags, trans->cache, trans->pkey);

    if (!map_res) [[unlikely]] {
      [[maybe_unused]] auto _ = unmap_range(dest_virt, size_bytes - remaining);
      return std::unexpected(map_res.error());
    }

    curr_src += chunk_bytes;
    curr_dest += chunk_bytes;
    remaining -= chunk_bytes;
  }

  return {};
}

[[nodiscard]] std::expected<void, Error> PageMap::move_virtual_range(VirtualAddress src_virt, VirtualAddress dest_virt,
                                                                     std::size_t size_bytes) noexcept {
  if (src_virt == dest_virt) {
    return {};
  }

  const auto alias_res = alias_virtual_range(src_virt, dest_virt, size_bytes);
  if (!alias_res) [[unlikely]] {
    return std::unexpected(alias_res.error());
  }

  const auto unmap_res = unmap_range(src_virt, size_bytes);
  if (!unmap_res) [[unlikely]] {
    [[maybe_unused]] auto _ = unmap_range(dest_virt, size_bytes);
    return std::unexpected(unmap_res.error());
  }

  return {};
}
} // namespace kernel::memory::vmm