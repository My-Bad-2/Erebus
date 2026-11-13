/// @file cap.hpp
/// @brief Capabilities: what one names, what it allows, and the objects behind them.
///
/// A capability is an entry in a CSpace (cspace.hpp): a pointer to a kernel object, a set of
/// rights, a few flags and a badge. Holding one is the only way to do anything to the object, so
/// what a thread may do is exactly what its CSpace holds, and handing out authority is handing out
/// capabilities, with fewer rights or a badge stamped on if need be.
///
/// ### Objects
///
/// Every object a capability can name is a CapObject, and counted twice:
///
///  - References keep the memory. Every capability slot holds one, and so does code that uses the
///    object outside an RCU read section, through a CapRef. The last one going frees the object,
///    after a grace period, as readers may still be looking at it, and in a kernel thread, as
///    tearing an object down may have to sleep (unregistering an interrupt handler does).
///  - Capabilities say whether anyone can still get at it. When the last one goes, the object is
///    told (on_unreachable()), even though references may remain: an endpoint wakes the threads
///    that wait on it, which nobody could ever reach otherwise.
///
/// An object made for user code also counts against a memory::Quota (see memory/quota.hpp), for
/// what it takes, from create_charged() until it is gone. An object that grows, a CSpace, charges
/// what it grows by to the same quota, and can fail to grow when there is no room left.
///
/// ### Revocation
///
/// A capability made by CSpace::insert() is the original. Copies and mints of it, and of those, are
/// derived, and carry the object's revocation epoch from when they were made. CSpace::revoke() on
/// an original moves the epoch on, and every derived capability of the object, in every CSpace,
/// stops working at once, without anyone having to find them. They are cleared out of the CSpace
/// the revoke came from right away; elsewhere they take up a slot until deleted, and do nothing.
#pragma once

#include <atomic>
#include <bitfield.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

#include "core/errors.hpp"
#include "memory/heap.hpp"
#include "memory/quota.hpp"
#include "utils/assert.hpp"

namespace kernel::core::ipc {
enum class CapType : std::uint8_t {
  Null = 0,          ///< An empty slot.
  Endpoint = 1,      ///< Synchronous IPC (ipc::Endpoint).
  Notification = 2,  ///< Asynchronous signals, a word of bits (ipc::Notification).
  Reply = 3,         ///< The right to answer one call, once (ipc::Reply).
  VmObject = 4,      ///< Memory that can be mapped into several address spaces (SharedVmObject).
  Mmio = 5,          ///< Device registers (ipc::MmioRegion).
  IrqHandler = 6,    ///< A device interrupt, delivered as a notification (ipc::IrqHandler).
  DmaPool = 7,       ///< A quota of pinned memory for devices to read and write (ipc::DmaPool).
  CSpace = 8,        ///< A capability space (ipc::CSpace).
  Thread = 9,        ///< A thread (ipc::ThreadCap).
  Process = 10,      ///< A process (ipc::ProcessCap).
};

enum class CapRights : std::uint8_t {
  None = 0,
  Read = 1 << 0,        ///< Receive on an endpoint, wait on a notification, map read-only.
  Write = 1 << 1,       ///< Send or call on an endpoint, signal a notification, map writable.
  Grant = 1 << 2,       ///< Send capabilities along with a message.
  GrantReply = 1 << 3,  ///< Let the reply to a call carry capabilities back.
  Mint = 1 << 4,        ///< Make copies with a new badge, or a part of the object.
  Revoke = 1 << 5,      ///< Revoke every capability derived from this original.
  Manage = 1 << 6,      ///< Configure the object: bind, acknowledge, pin, see where its memory
                        ///< is in RAM, and the like.
  Execute = 1 << 7,     ///< Map executable.

  All = 0xFF,
  ReadWrite = Read | Write,
  ClientCall = Write | GrantReply,
  ServerEndpoint = Read | Write | Grant | Mint | Revoke | Manage,
  DriverFull = Read | Write | Mint | Revoke | Manage,
};

[[nodiscard]] constexpr CapRights operator|(const CapRights a, const CapRights b) noexcept {
  return static_cast<CapRights>(std::to_underlying(a) | std::to_underlying(b));
}

[[nodiscard]] constexpr CapRights operator&(const CapRights a, const CapRights b) noexcept {
  return static_cast<CapRights>(std::to_underlying(a) & std::to_underlying(b));
}

[[nodiscard]] constexpr CapRights operator~(const CapRights a) noexcept {
  return static_cast<CapRights>(~std::to_underlying(a));
}

/// @brief Whether @p held has every right in @p required.
[[nodiscard]] constexpr bool has_rights(const CapRights held, const CapRights required) noexcept {
  return (std::to_underlying(held) & std::to_underlying(required)) == std::to_underlying(required);
}

enum class CapFlags : std::uint8_t {
  None = 0,
  OneShot = 1 << 0,   ///< Used once, by CSpace::consume(). Can be moved, never copied.
  Badged = 1 << 1,    ///< Carries a badge, which nobody can change any more.
  Pinned = 1 << 2,    ///< Cannot be deleted or moved; goes with its CSpace, or by revocation.
  Original = 1 << 3,  ///< Made by CSpace::insert(); revokes what was derived from it.
};

[[nodiscard]] constexpr CapFlags operator|(const CapFlags a, const CapFlags b) noexcept {
  return static_cast<CapFlags>(std::to_underlying(a) | std::to_underlying(b));
}

[[nodiscard]] constexpr CapFlags operator&(const CapFlags a, const CapFlags b) noexcept {
  return static_cast<CapFlags>(std::to_underlying(a) & std::to_underlying(b));
}

[[nodiscard]] constexpr CapFlags operator~(const CapFlags a) noexcept {
  return static_cast<CapFlags>(~std::to_underlying(a));
}

[[nodiscard]] constexpr bool has_cap_flag(const CapFlags flags, const CapFlags test) noexcept {
  return (std::to_underlying(flags) & std::to_underlying(test)) == std::to_underlying(test);
}

/// @brief Names a slot of a CSpace: its index, and the generation of the slot it was given out
///        for. A slot that was emptied and filled again has moved on, so an old ID names nothing.
class CapId {
  std::uint32_t m_data{0};

 public:
  static constexpr std::uint32_t INDEX_BITS = 20;
  static constexpr std::uint32_t GEN_BITS = 12;
  static constexpr std::uint32_t MAX_SLOTS = 1 << INDEX_BITS;
  static constexpr std::uint16_t MAX_GEN = (1 << GEN_BITS) - 1;

  constexpr CapId() noexcept = default;
  constexpr explicit CapId(const std::uint32_t raw) noexcept : m_data(raw) {}

  [[nodiscard]] static constexpr CapId make(const std::uint32_t index,
                                            const std::uint16_t generation) noexcept {
    CapId id{};
    id.set_index(index);
    id.set_generation(generation);
    return id;
  }

  [[nodiscard]] static constexpr CapId null() noexcept { return CapId{0}; }

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return m_data; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return m_data == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_data != 0; }

  BF_RW(std::uint32_t, index, 0, INDEX_BITS)
  BF_RW(std::uint16_t, generation, INDEX_BITS, GEN_BITS)

  [[nodiscard]] constexpr bool operator==(const CapId&) const noexcept = default;
};

inline constexpr CapId INVALID_CAP_ID = CapId::null();

class CSpace;

/// @brief What every object a capability can name derives from. See the file comment.
///
/// Objects are made with TypedCapObject::create(), which hands back the one reference there is in
/// a CapRef, and freed with kfree() once the last reference has gone.
class alignas(16) CapObject {
 public:
  using Hook = void (*)(CapObject* object) noexcept;

  CapObject(const CapObject&) = delete;
  CapObject& operator=(const CapObject&) = delete;

  [[nodiscard]] constexpr CapType cap_type() const noexcept { return m_type; }

  [[nodiscard]] std::uint32_t ref_count() const noexcept {
    return m_refs.load(std::memory_order_acquire);
  }

  /// @brief How many capability slots name the object right now.
  [[nodiscard]] std::uint32_t cap_count() const noexcept {
    return m_caps.load(std::memory_order_acquire);
  }

  /// @brief Moves on with every revoke. Derived capabilities from an older epoch are dead.
  [[nodiscard]] std::uint32_t epoch() const noexcept {
    return m_epoch.load(std::memory_order_acquire);
  }

  /// @brief One more reference. @pre The caller has one already.
  void add_ref() noexcept {
    [[maybe_unused]] const std::uint32_t previous = m_refs.fetch_add(1, std::memory_order_relaxed);
    KASSERT(previous > 0);
  }

  /// @brief One more reference, unless the object is on its way out already: what code that found
  ///        it in an RCU read section uses to keep it beyond the section.
  [[nodiscard]] bool try_add_ref() noexcept {
    std::uint32_t current = m_refs.load(std::memory_order_relaxed);
    while (current != 0) {
      if (m_refs.compare_exchange_weak(current, current + 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
        return true;
      }
    }

    return false;
  }

  /// @brief Lets go of a reference. The last one schedules the object's end: a grace period, then
  ///        its destructor in a kernel thread. Fine from any context but an NMI.
  void release_ref() noexcept;

  /// @brief The quota the object counts against, or null for one the kernel made for itself.
  [[nodiscard]] memory::Quota* charged_to() const noexcept { return m_charge.quota(); }

  /// @brief How much of it the object counts, so far.
  [[nodiscard]] std::size_t charged() const noexcept { return m_charge.bytes(); }

 protected:
  constexpr CapObject(const CapType type, const Hook destroy, const Hook unreachable) noexcept
      : m_type(type), m_destroy(destroy), m_unreachable(unreachable) {}

  // Gives back whatever the object was charged, as it is about to be freed.
  ~CapObject() noexcept = default;

  /// @brief Takes over @p charge, which the object then holds until it is gone.
  void adopt_charge(memory::QuotaCharge&& charge) noexcept { m_charge = std::move(charge); }

  /// @brief Charges @p bytes more to the object's quota, for an object that grows. Always fine for
  ///        one without a quota. @return QuotaExceeded, with nothing charged, past the limit.
  [[nodiscard]] std::expected<void, Error> charge_more(const std::size_t bytes) noexcept {
    return m_charge.grow(bytes);
  }

  /// @brief Gives back @p bytes of what charge_more() took, for growth that did not happen.
  void uncharge(const std::size_t bytes) noexcept { m_charge.shrink(bytes); }

 private:
  friend class CSpace;
  friend void reclaim_now() noexcept;

  // What release_ref() starts: after the grace period, onto the list of dead objects, and the
  // worker that destroys them told.
  static void after_grace_period(void* object) noexcept;

  // A capability slot now names the object. @pre The slot holds a reference of its own.
  void add_cap() noexcept { m_caps.fetch_add(1, std::memory_order_relaxed); }

  // A slot that named it was cleared. Tells the object if that was the last one.
  void drop_cap() noexcept {
    if (m_caps.fetch_sub(1, std::memory_order_acq_rel) == 1 && m_unreachable != nullptr) {
      m_unreachable(this);
    }
  }

  std::uint32_t bump_epoch() noexcept {
    return m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  std::atomic<std::uint32_t> m_refs{1};
  std::atomic<std::uint32_t> m_caps{0};
  std::atomic<std::uint32_t> m_epoch{0};
  CapType m_type;
  Hook m_destroy;
  Hook m_unreachable;
  CapObject* m_next_dead{nullptr};  ///< On the list of objects waiting to be destroyed.
  memory::QuotaCharge m_charge{};   ///< What the object takes, for user code that made it.
};

/// @brief An owned reference to a CapObject, let go of when this goes. Move-only.
template <typename T>
class [[nodiscard]] CapRef {
  T* m_object{nullptr};

 public:
  constexpr CapRef() noexcept = default;

  /// @brief Takes over the reference @p object comes with.
  [[nodiscard]] static CapRef adopt(T* object) noexcept {
    CapRef ref;
    ref.m_object = object;
    return ref;
  }

  /// @brief Takes a reference of its own. @pre The caller has one.
  [[nodiscard]] static CapRef share(T* object) noexcept {
    if (object != nullptr) {
      object->add_ref();
    }

    return adopt(object);
  }

  CapRef(CapRef&& other) noexcept : m_object(std::exchange(other.m_object, nullptr)) {}

  CapRef& operator=(CapRef&& other) noexcept {
    if (this != &other) {
      reset();
      m_object = std::exchange(other.m_object, nullptr);
    }

    return *this;
  }

  CapRef(const CapRef&) = delete;
  CapRef& operator=(const CapRef&) = delete;

  ~CapRef() noexcept { reset(); }

  void reset() noexcept {
    if (T* object = std::exchange(m_object, nullptr)) {
      object->release_ref();
    }
  }

  /// @brief Hands the reference to the caller, who lets go of it later.
  [[nodiscard]] T* leak() noexcept { return std::exchange(m_object, nullptr); }

  [[nodiscard]] T* get() const noexcept { return m_object; }
  T* operator->() const noexcept { return m_object; }
  T& operator*() const noexcept { return *m_object; }
  explicit operator bool() const noexcept { return m_object != nullptr; }
};

/// @brief The base of a concrete object type: its type tag, how it is made, and how it ends.
///
/// @p Derived may have `void on_unreachable() noexcept`, called when the last capability naming
/// it goes. It runs wherever that happened, and must not sleep.
template <typename Derived, CapType TypeTag>
class TypedCapObject : public CapObject {
  static void destroy_thunk(CapObject* base) noexcept {
    auto* self = static_cast<Derived*>(base);
    std::destroy_at(self);
    memory::heap::kfree(self);
  }

  static void unreachable_thunk(CapObject* base) noexcept {
    static_cast<Derived*>(base)->on_unreachable();
  }

  [[nodiscard]] static constexpr Hook unreachable_hook() noexcept {
    if constexpr (requires(Derived& d) { d.on_unreachable(); }) {
      return &unreachable_thunk;
    } else {
      return nullptr;
    }
  }

 protected:
  constexpr TypedCapObject() noexcept : CapObject(TypeTag, &destroy_thunk, unreachable_hook()) {}

 public:
  static constexpr CapType TYPE = TypeTag;

  /// @brief Allocates and constructs a @p Derived, with the one reference there is in the result.
  template <typename... Args>
  [[nodiscard]] static std::expected<CapRef<Derived>, Error> create(Args&&... args) noexcept {
    void* memory = memory::heap::kmalloc(sizeof(Derived), alignof(Derived));
    if (memory == nullptr) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    return CapRef<Derived>::adopt(
        std::construct_at(static_cast<Derived*>(memory), std::forward<Args>(args)...));
  }

  /// @brief create(), with the object counted against @p quota until it is gone. Null for none,
  ///        which is what objects the kernel makes for itself get.
  /// @return QuotaExceeded, with nothing made, if the quota has no room for it.
  template <typename... Args>
  [[nodiscard]] static std::expected<CapRef<Derived>, Error> create_charged(
      memory::Quota* const quota, Args&&... args) noexcept {
    auto charge = memory::QuotaCharge::take(quota, sizeof(Derived));
    if (!charge) [[unlikely]] {
      return std::unexpected(charge.error());
    }

    auto made = create(std::forward<Args>(args)...);
    if (made) [[likely]] {
      (*made)->adopt_charge(std::move(*charge));
    }

    return made;
  }
};

/// @brief The first word of a capability slot: type, rights, flags, the low bits of the slot's
///        generation, and the object's address, packed.
///
/// Objects are 16-byte aligned and the address is canonical, so bits 4 to 47 are all there is to
/// it. That only holds with 4-level paging; is_representable() says whether an address fits, and
/// CSpace refuses one that does not, rather than storing something else.
class CapHeader {
  std::uint64_t m_data{0};

  static constexpr std::uint32_t PTR_SHIFT = 4;
  static constexpr std::uint32_t PTR_BITS = 44;

 public:
  constexpr CapHeader() noexcept = default;
  constexpr explicit CapHeader(const std::uint64_t raw) noexcept : m_data(raw) {}

  [[nodiscard]] static bool is_representable(const void* const object) noexcept {
    const auto raw = reinterpret_cast<std::uintptr_t>(object);
    const auto sign_extended =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(raw << 16) >> 16);
    return (raw & 0xFul) == 0 && sign_extended == raw;
  }

  /// @pre is_representable(@p object).
  [[nodiscard]] static CapHeader make(const CapType type, const CapRights rights,
                                      const CapFlags flags, const std::uint16_t generation,
                                      const void* const object) noexcept {
    KASSERT(is_representable(object));
    CapHeader header{};
    header.set_type(type);
    header.set_rights(rights);
    header.set_flags(flags);
    header.set_gen_low(static_cast<std::uint8_t>(generation & 0xFu));
    header.set_ptr_bits((reinterpret_cast<std::uintptr_t>(object) >> PTR_SHIFT) &
                        ((std::uint64_t{1} << PTR_BITS) - 1));
    return header;
  }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return get_type() == CapType::Null; }

  BF_RW(CapType, type, 0, 4)
  BF_RW(CapRights, rights, 4, 8)
  BF_RW(CapFlags, flags, 12, 4)
  BF_RW(std::uint8_t, gen_low, 16, 4)
  BF_RW(std::uint64_t, ptr_bits, 20, 44)

  /// @brief The object's address: the packed bits back where they were, sign-extended from 47.
  [[nodiscard]] CapObject* object() const noexcept {
    const std::uint64_t bits = get_ptr_bits();
    if (bits == 0) [[unlikely]] {
      return nullptr;
    }

    const auto address = static_cast<std::uint64_t>(static_cast<std::int64_t>(bits << 20) >> 16);
    return reinterpret_cast<CapObject*>(address);
  }
};

static_assert(sizeof(CapHeader) == sizeof(std::uint64_t));

/// @brief A capability as a lookup found it. The object stays as long as the read section it was
///        found in, or the reference taken on it.
struct CapEntry {
  CapHeader header{};
  std::uint64_t badge{0};

  [[nodiscard]] constexpr bool is_valid() const noexcept { return !header.is_null(); }
  [[nodiscard]] constexpr CapType type() const noexcept { return header.get_type(); }
  [[nodiscard]] constexpr CapRights rights() const noexcept { return header.get_rights(); }
  [[nodiscard]] constexpr CapFlags flags() const noexcept { return header.get_flags(); }
  [[nodiscard]] CapObject* object() const noexcept { return header.object(); }

  [[nodiscard]] constexpr bool has(const CapRights required) const noexcept {
    return has_rights(rights(), required);
  }

  /// @brief The object as @p T. @pre type() is T::TYPE.
  template <typename T>
  [[nodiscard]] T* as() const noexcept {
    KASSERT(type() == T::TYPE);
    return static_cast<T*>(object());
  }
};

/// @brief A capability looked up and held: the object, with a reference of its own, and what the
///        capability said about it.
template <typename T>
struct Held {
  CapRef<T> object;
  CapRights rights{CapRights::None};
  CapFlags flags{CapFlags::None};
  std::uint64_t badge{0};

  [[nodiscard]] constexpr bool has(const CapRights required) const noexcept {
    return has_rights(rights, required);
  }
};

/// @brief Sets up what capabilities need: the cache of CSpace tables, and the thread that destroys
///        dead objects. @pre The heap and the workqueues work. Called once, at boot.
void initialize() noexcept;

/// @brief Destroys whatever has died so far, here and now, rather than waiting for the worker.
///        What tests use to see the end of an object. @pre The caller can sleep.
void reclaim_now() noexcept;
}  // namespace kernel::core::ipc
