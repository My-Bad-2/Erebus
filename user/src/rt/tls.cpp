#include "tls.hpp"

#include <string.h>

#include <bit>
#include <cstdint>
#include <new>

#include "sys/syscalls.hpp"

extern "C" {
[[gnu::visibility("hidden")]] extern const std::byte __tdata_start[];
[[gnu::visibility("hidden")]] extern const std::byte __tdata_end[];
[[gnu::visibility("hidden")]] extern const std::byte __tbss_end[];
[[gnu::visibility("hidden")]] extern const std::byte __tls_align[];
}

namespace user::rt::tls {
namespace {
constexpr std::size_t PAGE_SIZE = 4096;

[[nodiscard]] constexpr std::size_t align_up(const std::size_t value,
                                             const std::size_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

struct Layout {
  std::size_t file_bytes;   ///< What the template starts with, copied as it is.
  std::size_t tls_bytes;    ///< The whole template, rounded up to its own alignment.
  std::size_t tp_offset;    ///< Where tp is in the block; the template ends there.
  std::size_t block_bytes;  ///< Up to the end of the Tcb, in whole pages.
};

[[nodiscard]] Layout layout() noexcept {
  const auto start = reinterpret_cast<std::uintptr_t>(__tdata_start);
  const auto alignment = reinterpret_cast<std::uintptr_t>(__tls_align);

  const std::size_t tls_bytes =
      align_up(reinterpret_cast<std::uintptr_t>(__tbss_end) - start, alignment);
  const std::size_t tp_offset = align_up(tls_bytes, alignof(Tcb));
  return Layout{
      .file_bytes = reinterpret_cast<std::uintptr_t>(__tdata_end) - start,
      .tls_bytes = tls_bytes,
      .tp_offset = tp_offset,
      .block_bytes = align_up(tp_offset + sizeof(Tcb), PAGE_SIZE),
  };
}
}  // namespace

sys::Result<Tcb*> create() noexcept {
  const auto alignment = reinterpret_cast<std::uintptr_t>(__tls_align);
  if (!std::has_single_bit(alignment) || alignment > PAGE_SIZE) [[unlikely]] {
    return std::unexpected(sys::Error::InvalidAlignment);
  }

  const Layout layout = tls::layout();
  const auto block = sys::mem::allocate(layout.block_bytes, sys::AccessFlags::ReadWrite);
  if (!block) [[unlikely]] {
    return std::unexpected(block.error());
  }

  auto* const base = static_cast<std::byte*>(*block);
  klib::memcpy(base + (layout.tp_offset - layout.tls_bytes), __tdata_start, layout.file_bytes);

  auto* const tcb = new (base + layout.tp_offset) Tcb{
      .self = nullptr,
      .block = base,
      .block_bytes = layout.block_bytes,
      .entry = nullptr,
      .context = nullptr,
      .thread = sys::NULL_CAP,
      .done = sys::NULL_CAP,
      .failure = sys::Error::Success,
  };

  tcb->self = tcb;
  return tcb;
}

void destroy(Tcb* const tcb) noexcept { (void)sys::mem::unmap(tcb->block); }

sys::Result<void> install(Tcb* const tcb) noexcept { return sys::arch::set_fs_base(tcb); }
}  // namespace user::rt::tls
