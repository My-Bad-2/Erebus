#include "hal/hal.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/interrupts/interrupts.hpp"
#include "hal/user.hpp"
#include "register_asm.hpp"
#include "sched/scheduler.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::interrupts {
namespace {
constexpr std::size_t SAVED_REGISTERS_BYTES = sizeof(RegisterContext);
constexpr std::size_t VECTOR_AND_ERROR_BYTES = 2 * sizeof(std::uint64_t);
constexpr std::size_t CS_BEFORE_PUSHES = VECTOR_AND_ERROR_BYTES + sizeof(std::uint64_t);
constexpr std::size_t HARDWARE_FRAME_BYTES = 5 * sizeof(std::uint64_t);
constexpr std::size_t NMI_PERCPU_SLOT =
    SAVED_REGISTERS_BYTES + VECTOR_AND_ERROR_BYTES + HARDWARE_FRAME_BYTES;
static_assert(SAVED_REGISTERS_BYTES == 15 * sizeof(std::uint64_t));

constexpr std::uint32_t IA32_GS_BASE = 0xC0000101;

// The vector FRED gives SYSCALL; SYSENTER's is 2.
constexpr std::uint8_t FRED_SYSCALL_VECTOR = 1;

// Bit 1 of a #GP error code: the selector part is an IDT vector.
constexpr std::uint16_t IDT_ERROR_CODE = 1u << 1;
}  // namespace

[[gnu::naked, gnu::used]] void legacy_isr_common() noexcept {
  asm volatile(
      "testb $3, %c[cs](%%rsp)\n\t"
      "jz 1f\n\t"
      "swapgs\n\t"
      "1:\n\t"

      ASM_PUSH_ALL_REGISTERS

      "cld\n\t"
      "movq %%rsp, %%rdi\n\t"
      "call %P[dispatch]\n\t"

      ASM_POP_ALL_REGISTERS

      "testb $3, %c[cs](%%rsp)\n\t"
      "jz 2f\n\t"
      "swapgs\n\t"
      "2:\n\t"
      "addq $16, %%rsp\n\t"
      "iretq\n\t"
      : /* No Output */
      : [dispatch] "s"(&dispatch_legacy), [cs] "i"(CS_BEFORE_PUSHES)
      : "memory");
}

[[gnu::naked, gnu::used]] void legacy_nmi_common() noexcept {
  asm volatile(ASM_PUSH_ALL_REGISTERS

               "cld\n\t"
               "movl %[gs_msr], %%ecx\n\t"
               "rdmsr\n\t"
               "shlq $32, %%rdx\n\t"
               "orq %%rax, %%rdx\n\t"
               "movq %%rdx, %%r12\n\t"
               "movq %c[slot](%%rsp), %%rax\n\t"
               "cmpq %%rax, %%r12\n\t"
               "je 1f\n\t"
               "movq %%rax, %%rdx\n\t"
               "shrq $32, %%rdx\n\t"
               "wrmsr\n\t"
               "1:\n\t"

               "movq %%rsp, %%rdi\n\t"
               "call %P[dispatch]\n\t"

               "cmpq %c[slot](%%rsp), %%r12\n\t"
               "je 2f\n\t"
               "movl %[gs_msr], %%ecx\n\t"
               "movl %%r12d, %%eax\n\t"
               "movq %%r12, %%rdx\n\t"
               "shrq $32, %%rdx\n\t"
               "wrmsr\n\t"
               "2:\n\t"

               ASM_POP_ALL_REGISTERS

               "addq $16, %%rsp\n\t"
               "iretq\n\t"
               : /* No Output */
               : [dispatch] "s"(&dispatch_legacy_nmi), [slot] "i"(NMI_PERCPU_SLOT),
                 [gs_msr] "i"(IA32_GS_BASE)
               : "memory");
}

[[gnu::section(".text.fred"), gnu::aligned(4096)]] void fred_entrypoint() noexcept {
  asm volatile(
      "0:\n\t"
      "jmp 1f\n\t"
      ".org 0b + 256, 0xcc\n\t"
      "1:\n\t"
      "pushq $0\n\t"  // Keeps the stack 16-byte aligned for the call

      ASM_PUSH_ALL_REGISTERS

      "cld\n\t"
      "movq %%rsp, %%rdi\n\t"
      "call %P0\n\t"

      ASM_POP_ALL_REGISTERS

      "addq $8, %%rsp\n\t"
      "testb $3, 16(%%rsp)\n\t"  // CPL of the interrupted context, from the saved CS
      "jnz 2f\n\t"
      "erets\n\t"
      "2:\n\t"
      "eretu\n\t"
      : /* No Output */
      : "s"(&dispatch_fred)
      : "memory");
}

namespace {
template <std::size_t Vector>
  requires(Vector == std::to_underlying(InterruptVector::NonMaskableInterrupt))
[[gnu::naked, gnu::used]] void legacy_stub() noexcept {
  asm volatile(
      "pushq $0\n\t"
      "pushq %0\n\t"
      "jmp %P1\n\t"
      : /* No Output */
      : "i"(Vector), "s"(&legacy_nmi_common)
      : "memory");
}

template <std::size_t Vector>
  requires(Vector != std::to_underlying(InterruptVector::NonMaskableInterrupt) &&
           !has_error_code(InterruptVector{Vector}))
[[gnu::naked, gnu::used]] void legacy_stub() noexcept {
  asm volatile(
      "pushq $0\n\t"
      "pushq %0\n\t"
      "jmp %P1\n\t"
      : /* No Output */
      : "i"(Vector), "s"(&legacy_isr_common)
      : "memory");
}

template <std::size_t Vector>
  requires(Vector != std::to_underlying(InterruptVector::NonMaskableInterrupt) &&
           has_error_code(InterruptVector{Vector}))
[[gnu::naked, gnu::used]] void legacy_stub() noexcept {
  asm volatile(
      "pushq %0\n\t"
      "jmp %P1\n\t"
      : /* No Output */
      : "i"(Vector), "s"(&legacy_isr_common)
      : "memory");
}

template <std::size_t... Is>
consteval auto generate_legacy_stubs(std::index_sequence<Is...>) {
  return std::array<StubPtr, IDT_SIZE>{legacy_stub<Is>...};
}

[[nodiscard]] Event translate_event(LegacyFrame* frame) noexcept {
  Event event{
      .category = EventClass::Unknown,
      .vector = static_cast<std::uint8_t>(frame->vector),
      .cpl = static_cast<EventCpl>(frame->cs & 0x3),
      .error_code = static_cast<std::uint16_t>(frame->error_code),
      .payload = 0,
      .rip = frame->rip,
      .rsp = frame->rsp,
      .rflags = frame->rflags,
      .gprs = &frame->gprs,
  };

  if (event.vector == 2) {
    event.category = EventClass::NMI;
  } else if (event.vector < 32) {
    event.category = EventClass::Exception;

    if (event.vector == 0xe) {
      event.payload = read::cr2();
    }
  } else {
    event.category = EventClass::Interrupt;
  }

  // System calls never come this way: SYSCALL has an entry of its own (hal/syscall.hpp), and every
  // gate refuses INT n from ring 3.
  return event;
}

[[nodiscard]] Event translate_event(FredFrame* frame) noexcept {
  Event event{
      .category = EventClass::Unknown,
      .vector = frame->event_info.get_vector(),
      .cpl = static_cast<EventCpl>(frame->cs_info.get_cs_selector() & 0x3),
      .error_code = static_cast<std::uint16_t>(frame->error_code),
      .payload = frame->event_data,
      .rip = frame->rip,
      .rsp = frame->rsp,
      .rflags = frame->rflags,
      .gprs = &frame->gprs,
  };

  // What the IDT gives INT n, INT3 and INTO from ring 3, as all of its gates are DPL 0: a #GP that
  // names the vector. FRED has no gates to check, so it is up to us to give the same.
  const auto refuse_from_user = [&event] {
    event.category = EventClass::Exception;
    event.error_code = static_cast<std::uint16_t>((event.vector << 3) | IDT_ERROR_CODE);
    event.vector = std::to_underlying(InterruptVector::GeneralProtectionFault);
  };

  switch (frame->event_info.get_type()) {
    case fred::EventType::ExternalInterrupt:
      event.category = EventClass::Interrupt;
      break;
    case fred::EventType::Nmi:
      event.category = EventClass::NMI;
      break;
    case fred::EventType::HardwareException:
    case fred::EventType::PrivSoftwareException:  // INT1, which no gate stops either
      event.category = EventClass::Exception;
      break;
    case fred::EventType::SoftwareException:  // INT3 and INTO
      if (event.cpl == EventCpl::Ring3) {
        refuse_from_user();
      } else {
        event.category = EventClass::Exception;
      }
      break;
    case fred::EventType::SoftwareInterrupt:  // INT n
      refuse_from_user();
      break;
    case fred::EventType::OtherEvent:
      // SYSCALL from 64-bit code is a system call. SYSENTER, and SYSCALL from compatibility mode,
      // get the #UD they would get without FRED, as there is nowhere for them to go.
      if (event.vector == FRED_SYSCALL_VECTOR && frame->event_info.get_long_mode()) [[likely]] {
        event.category = EventClass::Syscall;
      } else {
        event.category = EventClass::Exception;
        event.vector = std::to_underlying(InterruptVector::InvalidOpcode);
        event.error_code = 0;
      }
      break;
    default:
      event.category = EventClass::Unknown;
      break;
  }

  return event;
}
}  // namespace

std::array<StubPtr, IDT_SIZE> legacy_idt_stubs =
    generate_legacy_stubs(std::make_index_sequence<IDT_SIZE>{});

// From ring 3, the kernel first takes back what user code may have changed under it, and on the
// way back makes sure the thread that returns is still meant to.
void dispatch_legacy(LegacyFrame* frame) noexcept {
  const auto event = translate_event(frame);
  const bool from_user = event.cpl == EventCpl::Ring3;
  if (from_user) {
    user::clear_ac();
  }

  InterruptManager::route_event(event);

  if (from_user) {
    sched::Scheduler::return_to_user();
  }
}

// An NMI can arrive anywhere, ring 3 included, and must never switch threads.
void dispatch_legacy_nmi(LegacyFrame* frame) noexcept {
  const auto event = translate_event(frame);
  if (event.cpl == EventCpl::Ring3) {
    user::clear_ac();
  }

  InterruptManager::route_event(event);
}

void dispatch_fred(FredFrame* frame) noexcept {
  const auto event = translate_event(frame);
  const bool from_user = event.cpl == EventCpl::Ring3;
  if (from_user) {
    user::clear_ac();
  }

  InterruptManager::route_event(event);

  if (from_user && event.category != EventClass::NMI) {
    sched::Scheduler::return_to_user();
  }
}
}  // namespace kernel::hw::interrupts