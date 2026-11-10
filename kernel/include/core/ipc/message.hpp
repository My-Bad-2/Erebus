/// @file message.hpp
/// @brief What an IPC message is: a tag, a badge, six words in registers, and more in a buffer.
///
/// Six words and no capabilities go straight from one Message to the other. Anything more comes
/// from the sender's IpcBuffer and goes to the receiver's: up to 57 more words, and up to four
/// capabilities, which move from one CSpace to the other on the way.
#pragma once

#include <array>
#include <bitfield.hpp>
#include <cstdint>

#include "core/cap.hpp"
#include "core/errors.hpp"

namespace kernel::core::ipc {
enum class MsgFlags : std::uint8_t {
  None = 0,
  IsAsyncNotification = 1 << 0,  ///< Not a message: the bits of the receiver's bound notification.
  DonateSchedContext = 1 << 1,   ///< Reserved for scheduling-context donation, which is not there.
  KernelError = 1 << 2,          ///< Something went wrong on the way; `status` says what.
  ReplyRequired = 1 << 3,        ///< From call(): the sender waits for a reply.
};

[[nodiscard]] constexpr MsgFlags operator|(const MsgFlags a, const MsgFlags b) noexcept {
  return static_cast<MsgFlags>(std::to_underlying(a) | std::to_underlying(b));
}

[[nodiscard]] constexpr MsgFlags operator&(const MsgFlags a, const MsgFlags b) noexcept {
  return static_cast<MsgFlags>(std::to_underlying(a) & std::to_underlying(b));
}

[[nodiscard]] constexpr bool has_msg_flag(const MsgFlags flags, const MsgFlags test) noexcept {
  return (std::to_underlying(flags) & std::to_underlying(test)) == std::to_underlying(test);
}

class MsgTag {
  std::uint64_t m_data{0};

 public:
  static constexpr std::uint8_t MAX_INLINE_WORDS = 6;
  static constexpr std::uint8_t MAX_OVERFLOW_WORDS = 57;
  static constexpr std::uint8_t MAX_TOTAL_WORDS = MAX_INLINE_WORDS + MAX_OVERFLOW_WORDS;
  static constexpr std::uint8_t MAX_EXTRA_CAPS = 4;

  constexpr MsgTag() noexcept = default;
  constexpr explicit MsgTag(const std::uint64_t raw) noexcept : m_data(raw) {}

  [[nodiscard]] static constexpr MsgTag make(const std::uint32_t label,
                                             const std::uint8_t length = 0,
                                             const std::uint8_t extra_caps = 0,
                                             const MsgFlags flags = MsgFlags::None,
                                             const Error status = Error::Success) noexcept {
    MsgTag tag{};
    tag.set_length(length <= MAX_TOTAL_WORDS ? length : MAX_TOTAL_WORDS);
    tag.set_extra_caps(extra_caps <= MAX_EXTRA_CAPS ? extra_caps : MAX_EXTRA_CAPS);
    tag.set_flags(flags);
    tag.set_status(static_cast<std::uint16_t>(std::to_underlying(status)));
    tag.set_label(label);
    return tag;
  }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, length, 0, 7)
  BF_RW(std::uint8_t, extra_caps, 7, 3)
  BF_RW(MsgFlags, flags, 10, 6)
  BF_RW(std::uint16_t, status, 16, 16)
  BF_RW(std::uint32_t, label, 32, 32)

  [[nodiscard]] constexpr Error error() const noexcept { return static_cast<Error>(get_status()); }

  /// @brief Marks the message as one that went wrong on the way, with @p status.
  constexpr void fail(const Error status) noexcept {
    set_flags(get_flags() | MsgFlags::KernelError);
    set_status(static_cast<std::uint16_t>(std::to_underlying(status)));
  }
  [[nodiscard]] constexpr bool is_fastpath_eligible() const noexcept {
    return get_length() <= MAX_INLINE_WORDS && get_extra_caps() == 0;
  }

  [[nodiscard]] constexpr bool operator==(const MsgTag&) const noexcept = default;
};

static_assert(sizeof(MsgTag) == sizeof(std::uint64_t));
static_assert(MsgTag::MAX_TOTAL_WORDS < (1u << 7), "the length field holds every length");

struct CapTransferItem {
  CapId src_cap{INVALID_CAP_ID};          ///< Capability in sender's `CSpace` to transfer
  CapRights rights_mask{CapRights::All};  ///< Rights attenuation mask applied during transfer
  std::uint8_t move_cap{0};               ///< 1 = move (delete from sender), 0 = copy/mint
  std::uint64_t new_badge{0};             ///< Optional badge to stamp if minting (0 = preserve)
};
static_assert(sizeof(CapTransferItem) == 16);

struct alignas(64) Message {
  MsgTag tag{};
  std::uint64_t badge{0};  ///< Populated by kernel on receive with `ep_cap.badge` or IRQ bits
  std::array<std::uint64_t, MsgTag::MAX_INLINE_WORDS> regs{};

  constexpr Message() noexcept = default;

  constexpr explicit Message(const std::uint32_t label, const std::uint64_t r0 = 0,
                             const std::uint64_t r1 = 0, const std::uint64_t r2 = 0,
                             const std::uint64_t r3 = 0, const std::uint64_t r4 = 0,
                             const std::uint64_t r5 = 0) noexcept
      : tag(MsgTag::make(label, MsgTag::MAX_INLINE_WORDS)), regs{r0, r1, r2, r3, r4, r5} {}
};

static_assert(sizeof(Message) == 64);

/// @brief A thread's place for what does not fit in a Message. Kernel memory, for now: there is no
///        system call layer yet to copy one in from user space.
struct alignas(64) IpcBuffer {
  /// Message words 6 to 62.
  std::array<std::uint64_t, MsgTag::MAX_OVERFLOW_WORDS> overflow_words{};

  /// Outgoing capability transfer descriptors set by the sender before `send()`/`call()`.
  std::array<CapTransferItem, MsgTag::MAX_EXTRA_CAPS> send_caps{};

  /// Incoming capability handles populated by the kernel in the receiver's `CSpace`.
  std::array<CapId, MsgTag::MAX_EXTRA_CAPS> recv_caps{};

  /// Number of capabilities successfully installed in `recv_caps` during the last receive.
  std::uint8_t received_cap_count{0};
};
}  // namespace kernel::core::ipc