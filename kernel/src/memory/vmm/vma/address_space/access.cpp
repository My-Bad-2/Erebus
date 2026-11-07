#include <string.h>

#include <algorithm>

#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
namespace {
// Where the byte at @p addr lives in the direct map, faulting the page in for @p access first if
// the page tables do not allow that yet.
[[nodiscard]] std::expected<std::byte*, Error> reach(AddressSpace& space, PageMap& pagemap,
                                                     const VirtualAddress addr,
                                                     const AccessFlags access) noexcept {
  const auto allows = [access](const TranslationResult& found) {
    return !has_flag(access, AccessFlags::Write) || has_flag(found.flags, AccessFlags::Write);
  };

  auto found = pagemap.translate(addr);
  if (!found || !allows(*found)) {
    if (const auto faulted = space.handle_page_fault(addr, access); !faulted) {
      return std::unexpected(faulted.error());
    }

    found = pagemap.translate(addr);
    if (!found || !allows(*found)) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }
  }

  return DirectMap::phys_to_virt(found->phys_address).as<std::byte>();
}

// Walks [addr, addr + size) one piece per page, handing each piece's kernel address to @p fn.
template <typename Fn>
[[nodiscard]] std::expected<void, Error> for_each_piece(AddressSpace& space, PageMap& pagemap,
                                                        const VirtualAddress addr,
                                                        const std::size_t size,
                                                        const AccessFlags access,
                                                        Fn&& fn) noexcept {
  std::size_t done = 0;
  while (done < size) {
    const VirtualAddress at = addr + static_cast<VirtualAddress::OffsetType>(done);
    const std::size_t in_page = PAGE_SIZE - (at.value() & (PAGE_SIZE - 1));
    const std::size_t piece = std::min(in_page, size - done);

    const auto kernel_view = reach(space, pagemap, at, access);
    if (!kernel_view) {
      return std::unexpected(kernel_view.error());
    }

    fn(*kernel_view, done, piece);
    done += piece;
  }

  return {};
}
}  // namespace

std::expected<void, Error> AddressSpace::write_bytes(
    const VirtualAddress dest, const std::span<const std::byte> bytes) noexcept {
  return for_each_piece(*this, m_pagemap, dest, bytes.size(), AccessFlags::Write,
                        [&](std::byte* to, const std::size_t offset, const std::size_t length) {
                          klib::memcpy(to, bytes.data() + offset, length);
                        });
}

std::expected<void, Error> AddressSpace::read_bytes(const VirtualAddress src,
                                                    const std::span<std::byte> out) noexcept {
  return for_each_piece(
      *this, m_pagemap, src, out.size(), AccessFlags::Read,
      [&](const std::byte* from, const std::size_t offset, const std::size_t length) {
        klib::memcpy(out.data() + offset, from, length);
      });
}
}  // namespace kernel::memory::vmm
