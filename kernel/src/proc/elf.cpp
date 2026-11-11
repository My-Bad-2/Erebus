#include "proc/elf.hpp"

#include <algorithm>
#include <array>

#include "crypto/random.hpp"
#include "hal/user.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/maths.hpp"

namespace kernel::proc::elf {
namespace {
using memory::PAGE_SIZE;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;

constexpr Elf64_Sxword TAG_RELRSZ = 35;
constexpr Elf64_Sxword TAG_RELR = 36;
constexpr Elf64_Sxword TAG_RELRENT = 37;

constexpr std::size_t MAX_LOAD_SEGMENTS = 16;

// anywhere 2 MiB aligned in [4 MiB, 4 MiB + 64 GiB). That keeps page zero and what is near it
// unmapped, and stays well below where the address space hands out heap and mmap ranges.
constexpr std::uintptr_t PIE_LOW = std::uintptr_t{4} << 20;
constexpr std::uintptr_t PIE_SPAN = std::uintptr_t{64} << 30;
constexpr std::uintptr_t PIE_ALIGN = std::uintptr_t{2} << 20;
static_assert(PIE_LOW + PIE_SPAN < USER_SPACE_BASE);

constexpr AccessFlags LOADING = AccessFlags::User | AccessFlags::Read | AccessFlags::Write;

struct Segment {
  std::uint64_t vaddr;  // As linked, before the load bias.
  std::uint64_t memsz;
  std::uint64_t offset;
  std::uint64_t filesz;
  AccessFlags access;  // What it ends up with.
};

struct Plan {
  std::array<Segment, MAX_LOAD_SEGMENTS> segments{};
  std::size_t count{0};
  std::uint64_t low{~std::uint64_t{0}};  // The lowest page, as linked.
  std::uint64_t high{0};                 // Past the highest page, as linked.
  const Elf64_Phdr* dynamic{nullptr};
  const Elf64_Phdr* tls{nullptr};
  const Elf64_Phdr* relro{nullptr};
  const Elf64_Phdr* phdr{nullptr};
};

struct Mapped {
  VirtualAddress base{};
  std::size_t size{0};
  AccessFlags access{};
};

// @p count Ts at @p offset of @p image, read where they are, which the CPU needs aligned.
template <typename T>
[[nodiscard]] auto view(const std::span<const std::byte> image, const std::uint64_t offset,
                        const std::uint64_t count) noexcept
    -> std::expected<std::span<const T>, Error> {
  std::uint64_t bytes = 0;
  std::uint64_t end = 0;
  if (__builtin_mul_overflow(count, sizeof(T), &bytes) ||
      __builtin_add_overflow(offset, bytes, &end) || end > image.size()) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  const std::byte* const at = image.data() + offset;
  if (reinterpret_cast<std::uintptr_t>(at) % alignof(T) != 0) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  return std::span{reinterpret_cast<const T*>(at), static_cast<std::size_t>(count)};
}

// PF_R, PF_W and PF_X as access flags, never both writable and executable.
[[nodiscard]] constexpr auto access_of(const std::uint32_t p_flags) noexcept
    -> std::expected<AccessFlags, Error> {
  if ((p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  auto access = AccessFlags::User;
  if ((p_flags & PF_R) != 0) {
    access = access | AccessFlags::Read;
  }

  if ((p_flags & PF_W) != 0) {
    access = access | AccessFlags::Write;
  }

  if ((p_flags & PF_X) != 0) {
    access = access | AccessFlags::Execute;
  }

  return access;
}

[[nodiscard]] auto add_segment(Plan& plan, const Elf64_Phdr& ph,
                               const std::span<const std::byte> image) noexcept
    -> std::expected<void, Error> {
  if (plan.count == MAX_LOAD_SEGMENTS) [[unlikely]] {
    return std::unexpected(Error::NotImplemented);
  }

  std::uint64_t file_end = 0;
  std::uint64_t vend = 0;
  if (ph.p_filesz > ph.p_memsz || __builtin_add_overflow(ph.p_vaddr, ph.p_memsz, &vend))
      [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  if (__builtin_add_overflow(ph.p_offset, ph.p_filesz, &file_end) || file_end > image.size())
      [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  const auto access = access_of(ph.p_flags);
  if (!access) [[unlikely]] {
    return std::unexpected(access.error());
  }

  // In order, as the ELF specification has them, and each in pages of its own: two segments in
  // one page would both need what either asks for, which may well be W and X together.
  const std::uint64_t first_page = utils::maths::align_down(ph.p_vaddr, PAGE_SIZE);
  const std::uint64_t end_page = utils::maths::align_up(vend, PAGE_SIZE);
  if (end_page < vend || (plan.count != 0 && first_page < plan.high)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  plan.segments[plan.count++] = Segment{.vaddr = ph.p_vaddr,
                                        .memsz = ph.p_memsz,
                                        .offset = ph.p_offset,
                                        .filesz = ph.p_filesz,
                                        .access = *access};
  plan.low = std::min(plan.low, first_page);
  plan.high = end_page;
  return {};
}

[[nodiscard]] auto make_plan(const std::span<const Elf64_Phdr> phdrs,
                             const std::span<const std::byte> image) noexcept
    -> std::expected<Plan, Error> {
  Plan plan;
  for (const Elf64_Phdr& ph : phdrs) {
    switch (ph.p_type) {
      case PT_LOAD:
        if (ph.p_memsz != 0) {
          if (const auto added = add_segment(plan, ph, image); !added) [[unlikely]] {
            return std::unexpected(added.error());
          }
        }
        break;

      case PT_INTERP:  // Needs a dynamic linker, and there is none.
        return std::unexpected(Error::NotImplemented);

      case PT_GNU_STACK:  // Stacks are never executable.
        if ((ph.p_flags & PF_X) != 0) [[unlikely]] {
          return std::unexpected(Error::SecurityViolation);
        }
        break;

      case PT_DYNAMIC:
        plan.dynamic = &ph;
        break;
      case PT_TLS:
        plan.tls = &ph;
        break;
      case PT_GNU_RELRO:
        plan.relro = &ph;
        break;
      case PT_PHDR:
        plan.phdr = &ph;
        break;
      default:
        break;
    }
  }

  if (plan.count == 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  return plan;
}

// The difference between where the image is linked and where it goes.
[[nodiscard]] auto choose_bias(const Elf64_Ehdr& ehdr, const Plan& plan,
                               const VirtualAddress pie_base) noexcept
    -> std::expected<std::uintptr_t, Error> {
  std::uint64_t base = plan.low;
  if (ehdr.e_type == ET_DYN) {
    if (!pie_base.is_null()) {
      if (!pie_base.is_aligned(PAGE_SIZE)) [[unlikely]] {
        return std::unexpected(Error::InvalidAlignment);
      }

      base = pie_base.value();
    } else {
      base = PIE_LOW + crypto::random::below(PIE_SPAN / PIE_ALIGN) * PIE_ALIGN;
    }
  }

  // All of the image, wherever it goes, in user space. Every address the image has is somewhere
  // in [low, high), so this is the one check its segments need.
  if (!hw::user::is_user_range(VirtualAddress{base}, plan.high - plan.low)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  return base - plan.low;
}

// Where the file has the @p size bytes the image has at @p vaddr, as linked.
[[nodiscard]] auto file_offset_of(const Plan& plan, const std::uint64_t vaddr,
                                  const std::uint64_t size) noexcept
    -> std::expected<std::uint64_t, Error> {
  for (std::size_t i = 0; i < plan.count; ++i) {
    const Segment& s = plan.segments[i];
    if (vaddr >= s.vaddr && vaddr - s.vaddr <= s.filesz && size <= s.filesz - (vaddr - s.vaddr)) {
      return s.offset + (vaddr - s.vaddr);
    }
  }

  return std::unexpected(Error::OutOfBounds);
}

// Changes 64-bit words of the image once it is mapped, a page at a time. Relocations come in
// order of address, so most of them land in the page the one before did, which is read once and
// written back once rather than once for each.
class Patcher {
 public:
  explicit Patcher(memory::vmm::AddressSpace& space) noexcept : m_space(space) {}
  ~Patcher() noexcept { memory::heap::kfree(m_page); }

  Patcher(const Patcher&) = delete;
  Patcher& operator=(const Patcher&) = delete;

  [[nodiscard]] std::expected<void, Error> set(const std::uint64_t address,
                                               const std::uint64_t value) noexcept {
    return word(address).transform([value](std::uint64_t* word) { *word = value; });
  }

  [[nodiscard]] std::expected<void, Error> add(const std::uint64_t address,
                                               const std::uint64_t delta) noexcept {
    return word(address).transform([delta](std::uint64_t* word) { *word += delta; });
  }

  [[nodiscard]] std::expected<void, Error> flush() noexcept {
    if (!m_dirty) {
      return {};
    }

    m_dirty = false;
    return m_space.write_bytes(VirtualAddress{m_base}, std::span{m_page, PAGE_SIZE});
  }

 private:
  [[nodiscard]] std::expected<std::uint64_t*, Error> word(const std::uint64_t address) noexcept {
    // Relocated words are aligned; one that is not could straddle two pages.
    if (address % sizeof(std::uint64_t) != 0) [[unlikely]] {
      return std::unexpected(Error::InvalidAlignment);
    }

    const std::uint64_t base = utils::maths::align_down(address, PAGE_SIZE);
    if (base != m_base || m_page == nullptr) {
      if (const auto flushed = flush(); !flushed) [[unlikely]] {
        return std::unexpected(flushed.error());
      }

      if (m_page == nullptr) {
        m_page = static_cast<std::byte*>(memory::heap::kmalloc(PAGE_SIZE, PAGE_SIZE));
        if (m_page == nullptr) [[unlikely]] {
          return std::unexpected(Error::OutOfMemory);
        }
      }

      // Whatever the address space has there: the image loaded it, and nothing else did.
      if (const auto read = m_space.read_bytes(VirtualAddress{base}, std::span{m_page, PAGE_SIZE});
          !read) [[unlikely]] {
        m_base = ~std::uint64_t{0};
        return std::unexpected(read.error());
      }

      m_base = base;
    }

    m_dirty = true;
    return reinterpret_cast<std::uint64_t*>(m_page + (address - base));
  }

  memory::vmm::AddressSpace& m_space;
  std::byte* m_page{nullptr};
  std::uint64_t m_base{~std::uint64_t{0}};
  bool m_dirty{false};
};

// Applies the relocations of a position-independent image: relative ones only, in RELA or RELR
// form. Anything else needs symbols, or code to run first, and makes the image one this cannot
// load.
[[nodiscard]] auto relocate(memory::vmm::AddressSpace& space,
                            const std::span<const std::byte> image, const Plan& plan,
                            const std::uintptr_t bias) noexcept -> std::expected<void, Error> {
  const Elf64_Phdr& dynamic = *plan.dynamic;
  if (dynamic.p_filesz % sizeof(Elf64_Dyn) != 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const auto entries =
      view<Elf64_Dyn>(image, dynamic.p_offset, dynamic.p_filesz / sizeof(Elf64_Dyn));
  if (!entries) [[unlikely]] {
    return std::unexpected(entries.error());
  }

  std::uint64_t rela = 0;
  std::uint64_t rela_size = 0;
  std::uint64_t rela_entry = sizeof(Elf64_Rela);
  std::uint64_t relr = 0;
  std::uint64_t relr_size = 0;
  std::uint64_t relr_entry = sizeof(Elf64_Xword);
  for (const Elf64_Dyn& entry : *entries) {
    if (entry.d_tag == DT_NULL) {
      break;
    }

    const std::uint64_t value = entry.d_un.d_val;
    switch (entry.d_tag) {
      case DT_RELA:
        rela = value;
        break;
      case DT_RELASZ:
        rela_size = value;
        break;
      case DT_RELAENT:
        rela_entry = value;
        break;
      case TAG_RELR:
        relr = value;
        break;
      case TAG_RELRSZ:
        relr_size = value;
        break;
      case TAG_RELRENT:
        relr_entry = value;
        break;
      case DT_RELSZ:     // REL, without addends, which x86-64 does not use.
      case DT_PLTRELSZ:  // PLT relocations, which need symbols or IRELATIVE resolvers.
        if (value != 0) [[unlikely]] {
          return std::unexpected(Error::NotImplemented);
        }
        break;
      default:
        break;
    }
  }

  if (rela_entry != sizeof(Elf64_Rela) || rela_size % sizeof(Elf64_Rela) != 0 ||
      relr_entry != sizeof(Elf64_Xword) || relr_size % sizeof(Elf64_Xword) != 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  Patcher patcher{space};

  if (rela_size != 0) {
    const auto at = file_offset_of(plan, rela, rela_size);
    const auto relas = at.and_then([&](const std::uint64_t offset) {
      return view<Elf64_Rela>(image, offset, rela_size / sizeof(Elf64_Rela));
    });
    if (!relas) [[unlikely]] {
      return std::unexpected(relas.error());
    }

    for (const Elf64_Rela& r : *relas) {
      switch (ELF64_R_TYPE(r.r_info)) {
        case R_X86_64_NONE:
          break;
        case R_X86_64_RELATIVE:
          if (const auto done = patcher.set(r.r_offset + bias, bias + r.r_addend); !done)
              [[unlikely]] {
            return done;
          }
          break;
        default:
          return std::unexpected(Error::NotImplemented);
      }
    }
  }

  // RELR: an even entry is the address of a word to relocate, and the start of a run. An odd one
  // is a bitmap of the 63 words after the run so far, bit 1 for the first.
  if (relr_size != 0) {
    const auto at = file_offset_of(plan, relr, relr_size);
    const auto words = at.and_then([&](const std::uint64_t offset) {
      return view<Elf64_Xword>(image, offset, relr_size / sizeof(Elf64_Xword));
    });

    if (!words) [[unlikely]] {
      return std::unexpected(words.error());
    }

    std::uint64_t where = 0;
    for (const Elf64_Xword word : *words) {
      if ((word & 1) == 0) {
        where = word + bias;
        if (const auto done = patcher.add(where, bias); !done) [[unlikely]] {
          return done;
        }

        where += sizeof(std::uint64_t);
        continue;
      }

      for (std::uint64_t bits = word >> 1, at_word = where; bits != 0;
           bits >>= 1, at_word += sizeof(std::uint64_t)) {
        if ((bits & 1) != 0) {
          if (const auto done = patcher.add(at_word, bias); !done) [[unlikely]] {
            return done;
          }
        }
      }

      where += 63 * sizeof(std::uint64_t);
    }
  }

  return patcher.flush();
}

[[nodiscard]] bool is_executable(const Plan& plan, const std::uint64_t vaddr) noexcept {
  for (std::size_t i = 0; i < plan.count; ++i) {
    const Segment& s = plan.segments[i];
    if (memory::vmm::has_flag(s.access, AccessFlags::Execute) && vaddr >= s.vaddr &&
        vaddr - s.vaddr < s.memsz) {
      return true;
    }
  }

  return false;
}

struct ThreadNote {
  Process::ThreadSetup setup;
  void* context;
  sched::ThreadId tid{sched::INVALID_THREAD_ID};
};

std::expected<void, Error> note_thread(sched::Thread& thread, void* const raw) noexcept {
  auto& note = *static_cast<ThreadNote*>(raw);
  note.tid = thread.tid();
  return note.setup != nullptr ? note.setup(thread, note.context) : std::expected<void, Error>{};
}
}  // namespace

auto validate(const std::span<const std::byte> image) noexcept
    -> std::expected<const Elf64_Ehdr*, Error> {
  const auto header = view<Elf64_Ehdr>(image, 0, 1);
  if (!header) [[unlikely]] {
    return std::unexpected(header.error());
  }

  const Elf64_Ehdr& ehdr = header->front();
  if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
      ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
      ehdr.e_ident[EI_VERSION] != EV_CURRENT || ehdr.e_version != EV_CURRENT ||
      ehdr.e_machine != EM_X86_64 || ehdr.e_ehsize < sizeof(Elf64_Ehdr) ||
      (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) || ehdr.e_phnum == 0 ||
      ehdr.e_phentsize != sizeof(Elf64_Phdr)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  if (const auto phdrs = view<Elf64_Phdr>(image, ehdr.e_phoff, ehdr.e_phnum); !phdrs) [[unlikely]] {
    return std::unexpected(phdrs.error());
  }

  return &ehdr;
}

auto load(memory::vmm::AddressSpace& space, const std::span<const std::byte> image,
          const VirtualAddress pie_base) noexcept -> std::expected<LoadedImage, Error> {
  const auto header = validate(image);
  if (!header) [[unlikely]] {
    return std::unexpected(header.error());
  }

  const Elf64_Ehdr& ehdr = **header;
  const auto phdrs = view<Elf64_Phdr>(image, ehdr.e_phoff, ehdr.e_phnum);
  const auto plan = phdrs.and_then([&](const auto& table) { return make_plan(table, image); });
  if (!plan) [[unlikely]] {
    return std::unexpected(plan.error());
  }

  const auto bias = choose_bias(ehdr, *plan, pie_base);
  if (!bias) [[unlikely]] {
    return std::unexpected(bias.error());
  }

  if (!is_executable(*plan, ehdr.e_entry)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const bool is_pie = ehdr.e_type == ET_DYN;
  if (plan->tls != nullptr && plan->tls->p_filesz > plan->tls->p_memsz) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  // Everything checked. From here on, a failure takes back what was mapped.
  std::array<Mapped, MAX_LOAD_SEGMENTS> mapped{};
  std::size_t mapped_count = 0;
  const auto fail = [&](const Error error) -> std::expected<LoadedImage, Error> {
    while (mapped_count != 0) {
      (void)space.free(mapped[--mapped_count].base);
    }

    return std::unexpected(error);
  };

  // Writable to begin with, so that the file can be copied in and relocated; anonymous memory
  // that nothing touches beyond the file stays zero, which is what .bss wants.
  for (std::size_t i = 0; i < plan->count; ++i) {
    const Segment& s = plan->segments[i];
    const std::uint64_t first_page = utils::maths::align_down(s.vaddr, PAGE_SIZE);
    const std::size_t size = utils::maths::align_up(s.vaddr + s.memsz, PAGE_SIZE) - first_page;

    const auto base =
        space.alloc(size, LOADING, memory::vmm::CacheMode::WriteBack, memory::vmm::PageSize::Size4K,
                    memory::vmm::VMAType::Anonymous, 0, VirtualAddress{first_page + *bias});
    if (!base) [[unlikely]] {
      return fail(base.error());
    }

    mapped[mapped_count++] = Mapped{.base = *base, .size = size, .access = s.access};

    if (s.filesz != 0) {
      const auto copied =
          space.write_bytes(VirtualAddress{s.vaddr + *bias}, image.subspan(s.offset, s.filesz));
      if (!copied) [[unlikely]] {
        return fail(copied.error());
      }
    }
  }

  if (is_pie && plan->dynamic != nullptr) {
    if (const auto relocated = relocate(space, image, *plan, *bias); !relocated) [[unlikely]] {
      return fail(relocated.error());
    }
  }

  for (std::size_t i = 0; i < mapped_count; ++i) {
    if (mapped[i].access != LOADING) {
      if (const auto done = space.protect(mapped[i].base, mapped[i].size, mapped[i].access); !done)
          [[unlikely]] {
        return fail(done.error());
      }
    }
  }

  // What relocation needed writable and nothing else does, as far as whole pages of it go.
  if (plan->relro != nullptr) {
    const std::uint64_t start = utils::maths::align_down(plan->relro->p_vaddr + *bias, PAGE_SIZE);
    const std::uint64_t end =
        utils::maths::align_down(plan->relro->p_vaddr + plan->relro->p_memsz + *bias, PAGE_SIZE);
    if (start < end) {
      if (const auto done = space.protect(VirtualAddress{start}, end - start,
                                          AccessFlags::User | AccessFlags::Read);
          !done) [[unlikely]] {
        return fail(done.error());
      }
    }
  }

  LoadedImage result{
      .entry = VirtualAddress{ehdr.e_entry + *bias},
      .image_base = VirtualAddress{plan->low + *bias},
      .image_end = VirtualAddress{plan->high + *bias},
      .phnum = ehdr.e_phnum,
      .phent_size = ehdr.e_phentsize,
      .load_bias = *bias,
      .is_pie = is_pie,
  };

  if (plan->phdr != nullptr) {
    result.phdr_vaddr = VirtualAddress{plan->phdr->p_vaddr + *bias};
  } else {
    for (std::size_t i = 0; i < plan->count; ++i) {
      const Segment& s = plan->segments[i];
      const std::uint64_t table = static_cast<std::uint64_t>(ehdr.e_phnum) * ehdr.e_phentsize;
      if (ehdr.e_phoff >= s.offset && ehdr.e_phoff - s.offset <= s.filesz &&
          table <= s.filesz - (ehdr.e_phoff - s.offset)) {
        result.phdr_vaddr = VirtualAddress{s.vaddr + (ehdr.e_phoff - s.offset) + *bias};
        break;
      }
    }
  }

  if (plan->tls != nullptr) {
    result.tls = TlsTemplate{
        .address = VirtualAddress{plan->tls->p_vaddr + *bias},
        .file_size = plan->tls->p_filesz,
        .memory_size = plan->tls->p_memsz,
        .alignment = plan->tls->p_align,
    };
  }

  return result;
}

auto spawn_from_elf(const std::string_view name, const std::span<const std::byte> image,
                    const std::uint64_t arg, const sched::UserThreadCreateOptions& opts,
                    Process* const parent, const Process::ThreadSetup setup,
                    void* const setup_context) noexcept -> std::expected<SpawnedProcess, Error> {
  Process* const owner = parent != nullptr ? parent : current_process();
  const auto created = Process::create(name, parent);
  if (!created) [[unlikely]] {
    return std::unexpected(created.error());
  }

  // One that never got going is ended and collected again, rather than left as a zombie that
  // nobody is going to wait for.
  Process* const process = *created;
  const ProcessId pid = process->pid();
  const auto fail = [&](const Error error) -> std::expected<SpawnedProcess, Error> {
    process->exit(EXIT_FAULT);
    (void)owner->wait_child(pid);
    return std::unexpected(error);
  };

  // Nothing runs in it yet, so the address space stays while it is filled in.
  memory::vmm::AddressSpace* const space = process->address_space();
  if (space == nullptr) [[unlikely]] {
    return fail(Error::InvalidState);
  }

  const auto loaded = load(*space, image);
  if (!loaded) [[unlikely]] {
    return fail(loaded.error());
  }

  ThreadNote note{.setup = setup, .context = setup_context};
  const auto thread = process->spawn_thread(name, loaded->entry, arg, opts, &note_thread, &note);
  if (!thread) [[unlikely]] {
    return fail(thread.error());
  }

  return SpawnedProcess{.process = process, .tid = note.tid, .image = *loaded};
}
}  // namespace kernel::proc::elf
