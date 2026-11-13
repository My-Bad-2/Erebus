// The kernel's copies to and from an address space: what system calls use for user memory.
//
// A copy goes through the direct map, one page at a time, and each page is a reader in the sense
// of the class comment in vma.hpp. The page has to stay what the translation said it was until the
// copy is over: found without that, a page another thread unmaps meanwhile goes back to the
// allocator with the copy still on its way to it, and a page another thread forks meanwhile is
// shared with the child by the time the copy lands. Doing the lookup, the fault and the copy in
// the read section, or under the lock, that a page fault would use makes every writer wait.
#include <string.h>

#include <algorithm>

#include "memory/vmm/vma/vma.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::memory::vmm {
namespace {
[[nodiscard]] bool allows(const TranslationResult& found, const AccessFlags access) noexcept {
  return !has_flag(access, AccessFlags::Write) || has_flag(found.flags, AccessFlags::Write);
}
}  // namespace

auto AddressSpace::reach_locked(VMArea* vma, const VirtualAddress addr,
                                const AccessFlags access) noexcept
    -> std::expected<std::byte*, Error> {
  auto found = m_pagemap.translate(addr);
  if (!found || !allows(*found, access)) {
    if (const auto faulted = resolve_vma_fault_locked(vma, addr, access); !faulted) {
      return std::unexpected(faulted.error());
    }

    found = m_pagemap.translate(addr);
    if (!found || !allows(*found, access)) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }
  }

  return DirectMap::phys_to_virt(found->phys_address).as<std::byte>();
}

template <typename Use>
auto AddressSpace::with_page(const VirtualAddress addr, const AccessFlags access,
                             Use&& use) noexcept -> std::expected<void, Error> {
  const auto reach_and_use = [&](VMArea* vma) -> std::expected<void, Error> {
    return reach_locked(vma, addr, access).transform([&](std::byte* page) { use(page); });
  };

  // Twice at most: an address in no VMA may be right below a stack, which the second time round
  // has grown over it.
  for (int attempt = 0; attempt < 2; ++attempt) {
    {
      const utils::rcu::ReadGuard rcu;
      if (m_fast_faults.load(std::memory_order_acquire)) [[likely]] {
        if (VMArea* vma = find_vma(addr)) [[likely]] {
          return reach_and_use(vma);
        }
      }
    }

    {
      const utils::SharedPreemptGuard shared_guard(m_as_lock);
      open_fast_faults();
      if (VMArea* vma = find_vma(addr)) {
        return reach_and_use(vma);
      }
    }

    // Growing a stack takes a writer, which cannot be had in here. handle_page_fault() says no for
    // anything that is not right below one.
    if (attempt == 0) {
      if (const auto grown = handle_page_fault(addr, access); !grown) {
        return grown;
      }
    }
  }

  return std::unexpected(Error::NotMapped);
}

std::expected<void, Error> AddressSpace::write_bytes(
    const VirtualAddress dest, const std::span<const std::byte> bytes) noexcept {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const VirtualAddress at = dest + static_cast<VirtualAddress::OffsetType>(done);
    const std::size_t piece =
        std::min(PAGE_SIZE - (at.value() & (PAGE_SIZE - 1)), bytes.size() - done);

    const auto copied = with_page(at, AccessFlags::Write, [&](std::byte* to) {
      klib::memcpy(to, bytes.data() + done, piece);
    });
    if (!copied) {
      return copied;
    }

    done += piece;
  }

  return {};
}

std::expected<void, Error> AddressSpace::read_bytes(const VirtualAddress src,
                                                    const std::span<std::byte> out) noexcept {
  std::size_t done = 0;
  while (done < out.size()) {
    const VirtualAddress at = src + static_cast<VirtualAddress::OffsetType>(done);
    const std::size_t piece =
        std::min(PAGE_SIZE - (at.value() & (PAGE_SIZE - 1)), out.size() - done);

    const auto copied = with_page(at, AccessFlags::Read, [&](const std::byte* from) {
      klib::memcpy(out.data() + done, from, piece);
    });
    if (!copied) {
      return copied;
    }

    done += piece;
  }

  return {};
}
}  // namespace kernel::memory::vmm
