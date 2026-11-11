/// @file elf.hpp
/// @brief Loading ELF64 executables into user address spaces.
///
/// The image is the file as it is in memory, a Limine module say, and is only ever read: every
/// segment is copied into anonymous memory of the address space, which counts against the
/// process's quota like any other. Executables with a dynamic linker (PT_INTERP) are refused, as
/// there is none to hand them to, and so is anything that needs code to run before it can, such
/// as IRELATIVE relocations. Position-independent executables (ET_DYN) are relocated: their
/// relative relocations, in RELA or RELR form, are the only kind a static PIE has.
///
/// Every segment ends up with the permissions its header asks for, and never both writable and
/// executable. A PT_GNU_RELRO range is made read-only once relocations are done, and an
/// executable that wants an executable stack is refused.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "boot/elf.h"
#include "core/errors.hpp"
#include "memory/address.hpp"
#include "proc/process.hpp"
#include "sched/thread.hpp"

namespace kernel::proc::elf {
/// @brief Where the thread-local storage template is, for user code to set up TLS from.
struct TlsTemplate {
  memory::VirtualAddress address{};  ///< Of the initialised part, in the loaded image.
  std::size_t file_size{0};          ///< Initialised bytes (.tdata).
  std::size_t memory_size{0};        ///< All of it, zeroed after the initialised part (.tbss).
  std::size_t alignment{0};

  [[nodiscard]] constexpr bool present() const noexcept { return memory_size != 0; }
};

/// @brief What load() put where.
struct LoadedImage {
  memory::VirtualAddress entry{};
  memory::VirtualAddress image_base{};  ///< Lowest page of the image.
  memory::VirtualAddress image_end{};   ///< Past its highest page.
  memory::VirtualAddress phdr_vaddr{};  ///< The program headers, if they are in the image.
  std::uint16_t phnum{0};
  std::uint16_t phent_size{0};
  std::uintptr_t load_bias{0};  ///< Added to every address in the file: 0 for ET_EXEC.
  TlsTemplate tls{};
  bool is_pie{false};
};

/// @brief A process spawn_from_elf() started.
struct SpawnedProcess {
  /// Stays until its parent collects it with wait_child(); nothing else frees it.
  Process* process{nullptr};
  /// The ID of its thread. The thread itself may be gone by the time this is read.
  sched::ThreadId tid{sched::INVALID_THREAD_ID};
  LoadedImage image{};
};

/// @brief Checks that @p image is an ELF64 x86-64 executable whose program headers are all inside
///        it.
/// @return The header, inside @p image; InvalidArgument for anything else, OutOfBounds for
///         headers past its end, InvalidAlignment for a header the CPU could not read in place.
[[nodiscard]] auto validate(std::span<const std::byte> image) noexcept
    -> std::expected<const Elf64_Ehdr*, Error>;

/// @brief Maps every PT_LOAD segment of @p image into @p space, relocates it if it is
///        position-independent, and gives each segment its permissions.
///
/// Nothing is mapped until the program headers have all been checked; anything mapped by the time
/// something else fails is unmapped again.
/// @param pie_base Where the lowest page of a position-independent image goes. Null for a random,
///                 2 MiB aligned place in the first 64 GiB above 4 MiB. Unused for ET_EXEC.
/// @return What went where; InvalidArgument for program headers that do not make an executable,
///         OutOfBounds for segments outside user space or the file, SecurityViolation for a
///         segment or stack that would be writable and executable, NotImplemented for what this
///         cannot load, and whatever the address space says.
[[nodiscard]] auto load(memory::vmm::AddressSpace& space, std::span<const std::byte> image,
                        memory::VirtualAddress pie_base = {}) noexcept
    -> std::expected<LoadedImage, Error>;

/// @brief Creates a process, child of @p parent (null for the caller's), loads @p image into it,
///        and starts its thread at the entry point with @p arg in RDI.
///
/// @p setup runs on the thread before it may run, as for Process::spawn_thread(): the place to
/// give it an IPC context. A process that could not be started is ended and collected again.
[[nodiscard]] auto spawn_from_elf(std::string_view name, std::span<const std::byte> image,
                                  std::uint64_t arg = 0,
                                  const sched::UserThreadCreateOptions& opts = {},
                                  Process* parent = nullptr, Process::ThreadSetup setup = nullptr,
                                  void* setup_context = nullptr) noexcept
    -> std::expected<SpawnedProcess, Error>;
}  // namespace kernel::proc::elf
