/// @file vma.hpp
/// @brief Virtual memory areas and the address spaces made of them.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "memory/vmm/pagemap/flags.hpp"
#include "memory/vmm/pagemap/tlb.hpp"
#include "utils/bplus_tree.hpp"
#include "utils/lock.hpp"
#include "vm_object.hpp"

namespace kernel::memory::vmm {
enum class VMAType : std::uint8_t {
  Anonymous,
  FileBacked,
  SharedIPC,
  HardwareMMIO,
};

enum class NUMAPolicy : std::uint8_t {
  StrictLocal = 0,
  Interleave = 1,
  FirstTouch = 2,
};

/// @brief Packed attributes of a VMA.
class VMAFlags {
  std::uint64_t m_data;

 public:
  constexpr explicit VMAFlags(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  void clear() noexcept { m_data = 0; }

  BF_RW(AccessFlags, access, 0, 16)
  BF_RW(CacheMode, cache, 16, 3)
  BF_RW(PageSize, page_size, 19, 2)
  BF_RW(VMAType, type, 21, 4)
  BF_RW(NUMAPolicy, numa, 25, 3)

  BF_BIT_RW(is_locked, 28)  // Excluded from swap
  BF_BIT_RW(is_wired, 29)   // Pinned in memory and never faulted in (MMIO, aliases)

  BF_RW(std::uint8_t, pkey, 30, 4)  // Protection key
};

/// @brief A contiguous range `[start, end)` of an address space with uniform attributes.
///
/// Pages of the range come from `vm_object`, page `object_offset` onwards, in units of the VMA's
/// page size. Wired VMAs (MMIO, aliases) have no object: their page tables are filled when they
/// are created and they never take faults.
struct alignas(std::hardware_destructive_interference_size) VMArea {
  VirtualAddress start;
  VirtualAddress end;

  VMObject* vm_object;
  std::size_t object_offset;
  VMAFlags flags;

  VMArea* next_vma;
  VMArea* prev_vma;

  std::uint32_t preferred_numa_node;
  std::uint32_t fault_count;

  [[nodiscard]] bool contains(const VirtualAddress addr) const noexcept {
    return addr.value() >= start.value() && addr.value() < end.value();
  }

  [[nodiscard]] std::size_t size_bytes() const noexcept { return end.value() - start.value(); }

  [[nodiscard]] std::size_t page_bytes() const noexcept {
    return get_page_bytes(flags.get_page_size());
  }
};

/// @brief A set of non-overlapping VMAs plus the page tables that cache their translations.
///
/// VMAs are kept twice: in a B+ tree keyed by start address, for lookups, and in a sorted doubly
/// linked list, for walking neighbours. Whatever changes either, or the page tables under them, is
/// a writer and holds a WriteGuard. Whatever only looks holds `m_as_lock` shared (the page tables
/// and VM objects synchronise on their own), except page faults, the one reader that is on every
/// CPU at once.
///
/// A page fault takes no lock at all as long as the fast path is open: it finds its VMA and
/// resolves the fault in an RCU read section (utils/rcu/rcu.hpp). A writer closes the fast path,
/// waits out a grace period, after which every fault that got in has left, and only then takes
/// `m_as_lock`. Faults that come along meanwhile go the slow way, which waits for the lock, and the
/// first of them after the writers are done opens the fast path again. So the tree and the VMAs
/// never meet a reader while they change, and nothing in them has to be safe for one. Faults share
/// no cache line that anyone writes, and a burst of writers pays one grace period, for the first.
///
/// Anonymous memory is demand paged unless AccessFlags::Populate is given. Demand-paged memory
/// must not be touched with interrupts disabled: the fault may have to wait for a CPU that is
/// itself waiting for this one to answer a TLB shootdown.
///
/// New ranges are placed in one of two 32 TiB regions (one for "heap", one for "mmap" style
/// allocations). Each region has a bump pointer that moves up with a random gap after every
/// allocation, so freed ranges are not handed out again straight away. Once the bump pointer
/// reaches the end of its region the search starts over from the bottom and takes the first
/// hole that fits.
class AddressSpace {
  static inline heap::KmemCache* s_vma_cache = nullptr;

  /// How far a stack VMA may grow downwards.
  static constexpr std::size_t MAX_STACK_BYTES = 4 * PAGE_SIZE_2MB;

  // m_as_lock already serialises every access, so the tree goes without a lock of its own.
  utils::BPlusTree<std::uint64_t, VMArea*, 256, utils::NullLock> m_vma_tree;

  VMArea* m_vma_list_head{nullptr};
  VMArea* m_vma_list_tail{nullptr};

  PageMap m_pagemap;
  mutable utils::RWLock m_as_lock;

  // Whether page faults may take the fast path; see the class comment. Only a writer, holding
  // m_writers, closes it, and a slow fault opens it again once no writer is about. It starts out
  // closed: nobody can be faulting in a new address space, so whatever fills it in need not wait.
  std::atomic<bool> m_fast_faults{false};

  // Held by a writer from before it closes the fast path until it is done, so that writers take
  // turns and a fault can tell whether one is about. Faults never wait for it.
  utils::QSpinlock m_writers{};

  // What every writer holds: m_writers, the fast path closed and waited out, then m_as_lock
  // exclusively. Preemption is off throughout, as for the lock alone.
  class [[nodiscard]] WriteGuard {
   public:
    explicit WriteGuard(AddressSpace& space) noexcept;
    ~WriteGuard() noexcept;

    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;

   private:
    AddressSpace& m_space;
  };

  // Opens the fast path, unless a writer is about. @pre m_as_lock is held shared.
  void open_fast_faults() noexcept;

  // Where find_free_gap() hands out addresses: [start, limit), next try at bump.
  struct Region {
    VirtualAddress start;
    VirtualAddress limit;
    VirtualAddress bump;
  };

  Region m_heap_region;
  Region m_mmap_region;

  // What memory filled in on demand here counts against. See set_quota().
  QuotaRef m_quota{};

  [[nodiscard]] static VMArea* allocate_vma() noexcept;
  static void release_vma(VMArea* vma) noexcept;

  [[nodiscard]] VMArea* find_vma(VirtualAddress addr) const noexcept;
  [[nodiscard]] bool range_is_free(VirtualAddress start, VirtualAddress end) const noexcept;
  [[nodiscard]] VirtualAddress first_fit(VirtualAddress from, VirtualAddress limit,
                                         std::size_t size_bytes,
                                         std::size_t alignment) const noexcept;
  [[nodiscard]] VirtualAddress find_free_gap(std::size_t size_bytes, std::size_t alignment,
                                             bool is_heap = false) noexcept;
  void link_vma(VMArea* new_vma) noexcept;
  void unlink_vma(VMArea* vma) noexcept;
  void release_vma_locked(VMArea* vma) noexcept;
  auto split_vma(VMArea* vma, VirtualAddress split_point) noexcept -> std::expected<VMArea*, Error>;
  [[nodiscard]] auto isolate_range(VirtualAddress start, VirtualAddress end) noexcept
      -> std::expected<VMArea*, Error>;

  [[nodiscard]] static bool can_merge(const VMArea* left, const VMArea* right) noexcept;
  void absorb_next(VMArea* left) noexcept;
  void coalesce_range(VMArea* last, VirtualAddress start) noexcept;

  [[nodiscard]] auto expand_stack_locked(VirtualAddress fault_addr) noexcept
      -> std::expected<VMArea*, Error>;
  // @pre Writers are kept out: m_as_lock is held, or the fast path was open in the caller's read
  //      section (see handle_page_fault()).
  [[nodiscard]] auto resolve_vma_fault_locked(VMArea* vma, VirtualAddress fault_addr,
                                              AccessFlags fault_type) noexcept
      -> std::expected<void, Error>;

  // Where the byte at @p addr, in @p vma, is in the direct map, faulted in for @p access first if
  // the page tables do not allow that yet. @pre As for resolve_vma_fault_locked(), and for as long
  // as the caller uses the pointer.
  [[nodiscard]] auto reach_locked(VMArea* vma, VirtualAddress addr, AccessFlags access) noexcept
      -> std::expected<std::byte*, Error>;

  // Runs @p use on the direct-map view of the page at @p addr, with writers kept out from before
  // the page is looked up until @p use returns. See write_bytes().
  template <typename Use>
  [[nodiscard]] auto with_page(VirtualAddress addr, AccessFlags access, Use&& use) noexcept
      -> std::expected<void, Error>;

 public:
  /// @param base_addr Where the heap and mmap bump pointers start (plus random offsets).
  explicit AddressSpace(VirtualAddress base_addr) noexcept;

  /// @brief Unmaps and releases every VMA.
  ~AddressSpace() noexcept;

  AddressSpace(const AddressSpace&) = delete;
  AddressSpace& operator=(const AddressSpace&) = delete;

  /// @brief Sets up the slab caches for VMAs and VM objects. Called once by the VMM.
  static void initialize() noexcept;

  [[nodiscard]] PageMap& pagemap() noexcept { return m_pagemap; }

  /// @brief Has the memory this address space fills in on demand count against @p quota, null for
  ///        nothing, which is what the kernel's own gets.
  ///
  /// It is a commitment that counts, not a page in use: a new range all of it at once, a stack
  /// what it grows by when it does, and after a fork each copy-on-write side all it could come
  /// to copy. Each object holds what it was charged until it is gone. Device memory and
  /// existing objects mapped here count against whatever they did before, if anything.
  /// @pre Nothing was allocated yet.
  void set_quota(Quota* const quota) noexcept { m_quota = QuotaRef::share(quota); }

  [[nodiscard]] Quota* quota() const noexcept { return m_quota.get(); }

  /// @brief Reserves (and with Populate, backs) a new anonymous range.
  /// @param size_bytes Rounded up to @p page_size.
  /// @param hint       Exact address to use, or null to let the address space pick one.
  /// @return The start of the range. AlreadyMapped if @p hint overlaps an existing VMA,
  ///         QuotaExceeded if the range would take the quota past its limit.
  [[nodiscard]] auto alloc(std::size_t size_bytes, AccessFlags access,
                           CacheMode cache = CacheMode::WriteBack,
                           PageSize page_size = PageSize::Size4K, VMAType type = VMAType::Anonymous,
                           std::uint8_t pkey = 0, VirtualAddress hint = VirtualAddress{}) noexcept
      -> std::expected<VirtualAddress, Error>;

  /// @brief Removes the VMA starting exactly at @p addr.
  [[nodiscard]] auto free(VirtualAddress addr) noexcept -> std::expected<void, Error>;

  /// @brief Removes the VMA that contains @p addr, wherever it starts. A stack that has grown
  ///        starts lower than where it was made, so this is how one is found again.
  [[nodiscard]] auto free_containing(VirtualAddress addr) noexcept -> std::expected<void, Error>;

  /// @brief Grows the VMA starting at @p old_addr, moving it if the space after it is taken.
  ///
  /// Only works for VMAs backed by a VM object. A move just unmaps the old range: the pages stay
  /// in the object and fault back in at the new address.
  [[nodiscard]] auto realloc(VirtualAddress old_addr, std::size_t new_size) noexcept
      -> std::expected<VirtualAddress, Error>;

  /// @brief Changes the read/write/execute permissions of a range spanning one or more VMAs.
  ///
  /// The range must be fully covered by VMAs and aligned to their page size. VMAs are split at
  /// the edges as needed and merged again with identical neighbours afterwards. Only anonymous
  /// memory can gain a permission; anything else can only lose them.
  /// @return AccessDenied, with nothing changed, for a permission a VMA that is not anonymous
  ///         does not have.
  [[nodiscard]] auto protect(VirtualAddress addr, std::size_t size_bytes,
                             AccessFlags new_flags) noexcept -> std::expected<void, Error>;

  /// @brief Maps device memory. The VMA is wired: mapped right away and never faulted.
  auto map_mmio(PhysicalAddress phys_base, std::size_t size_bytes,
                AccessFlags access = AccessFlags::Read | AccessFlags::Write | AccessFlags::Mmio,
                CacheMode cache = CacheMode::Uncacheable, PageSize page_size = PageSize::Size4K,
                std::uint8_t pkey = 0, VirtualAddress hint = VirtualAddress{}) noexcept
      -> std::expected<VirtualAddress, Error>;

  /// @brief Faults in every page of the range and keeps it away from eviction.
  [[nodiscard]] auto lock_memory(VirtualAddress addr, std::size_t size_bytes) noexcept
      -> std::expected<void, Error>;

  /// @brief Undoes lock_memory() for the range.
  [[nodiscard]] auto unlock_memory(VirtualAddress addr, std::size_t size_bytes) noexcept
      -> std::expected<void, Error>;

  /// @brief Copies every VMA into the (empty) @p child, sharing private memory copy-on-write.
  ///
  /// All allocations happen before anything is changed, so on failure both address spaces are
  /// exactly as they were. On success private VMAs on both sides point at new, empty shadow
  /// objects of the original and the parent's pages are write-protected. Each shadow counts the
  /// whole range against its side's quota (see set_quota()).
  /// @return OutOfMemory, or QuotaExceeded if either side has no room for its shadows.
  [[nodiscard]] auto clone_into(AddressSpace& child) noexcept -> std::expected<void, Error>;

  /// @brief Maps an existing VM object, sharing its pages with every other mapping of it.
  [[nodiscard]] auto map_object(VMObject* existing_obj, std::size_t size_bytes,
                                std::size_t obj_offset_pages, AccessFlags access,
                                CacheMode cache = CacheMode::WriteBack,
                                PageSize page_size = PageSize::Size4K,
                                VMAType type = VMAType::SharedIPC, std::uint8_t pkey = 0,
                                VirtualAddress hint = VirtualAddress{}) noexcept
      -> std::expected<VirtualAddress, Error>;

  /// @brief Maps the frames behind [@p addr, @p addr + @p size_bytes) a second time.
  ///
  /// Pages that are not present yet are faulted in first. The alias is a wired VMA with its own
  /// permissions, typically used to get a writable view of read-only memory.
  [[nodiscard]] auto create_alias(VirtualAddress addr, std::size_t size_bytes,
                                  AccessFlags alias_flags) noexcept
      -> std::expected<VirtualAddress, Error>;
  [[nodiscard]] auto destroy_alias(const VirtualAddress alias_addr) noexcept
      -> std::expected<void, Error> {
    return free(alias_addr);
  }

  /// @brief Drops the translation of one page. It faults back in from its object when touched.
  [[nodiscard]] auto evict_page(VirtualAddress addr) noexcept -> std::expected<void, Error>;

  /// @brief Resolves a page fault, growing a stack VMA if the address sits right below one.
  ///
  /// Takes no lock while no writer is about; see the class comment. Growing a stack is a writer,
  /// which waits for read sections to end, so it cannot be done from inside one.
  /// @return An error if the access is not allowed or the address is not mapped by any VMA, and
  ///         DeadlockDetected for a stack that would have to grow inside a read section.
  [[nodiscard]] auto handle_page_fault(VirtualAddress fault_addr, AccessFlags fault_type) noexcept
      -> std::expected<void, Error>;

  /// @brief Copies @p bytes into this address space at @p dest, whether it is loaded or not.
  ///
  /// The kernel's own access to it: pages are faulted in as a write from the kernel would, which
  /// a user mapping allows, and copy-on-write pages get their copy first. A VMA that does not
  /// allow writing at all says no, as it would to anyone. The copy goes through the direct map,
  /// so SMAP is no obstacle.
  ///
  /// Other threads of the process may change the address space meanwhile, which is what makes
  /// this safe for system calls. Each page is a reader, as a page fault is: it is found, filled in
  /// and copied with writers kept out, so an unmap, a protect or a fork waits for the copy rather
  /// than freeing or sharing the page under it. A writer can still come between two pages: the
  /// copy then stops at the first page that is gone.
  /// @return The error of the first page that could not be faulted in. What came before it is
  ///         copied already.
  [[nodiscard]] auto write_bytes(VirtualAddress dest, std::span<const std::byte> bytes) noexcept
      -> std::expected<void, Error>;

  /// @brief The other way around: fills @p out from this address space at @p src.
  ///
  /// Pages that were never touched are faulted in as well, as a read from the kernel would.
  [[nodiscard]] auto read_bytes(VirtualAddress src, std::span<std::byte> out) noexcept
      -> std::expected<void, Error>;

  template <typename Func>
    requires std::invocable<Func, const VMArea&>
  void for_each_vma(Func&& callback) const noexcept {
    utils::SharedPreemptGuard guard(m_as_lock);

    for (const VMArea* curr = m_vma_list_head; curr; curr = curr->next_vma) {
      callback(*curr);
    }
  }
};

/// @brief The kernel's own address space, or null before vmm::initialize().
AddressSpace* kernel_space() noexcept;

/// @brief Finds the address space of the user code that is running, or null if there is none.
using UserSpaceLookup = AddressSpace* (*)() noexcept;

/// @brief Tells the page fault handler where user faults go.
///
/// Who runs what is up to the layers above this one, which is why they have to say: faults in the
/// lower half from ring 3 are resolved in whatever @p lookup returns, and fail without it.
void set_user_space_lookup(UserSpaceLookup lookup) noexcept;

/// @brief Maps @p size bytes of device registers at @p phys into the kernel, uncached.
///
/// Unlike AddressSpace::map_mmio(), @p phys need not be page aligned: the pages around it are
/// mapped and the returned address points at @p phys itself. Firmware tables describe register
/// blocks this way (an HPET at 0xFED00400, say).
[[nodiscard]] std::expected<VirtualAddress, Error> map_device_registers(PhysicalAddress phys,
                                                                        std::size_t size) noexcept;
}  // namespace kernel::memory::vmm
