#include "hal/hal.hpp"
#include "hal/interrupt_manager.hpp"
#include "hal/interrupts.hpp"

#define ASM_PUSH_ALL_REGISTERS                                                                                         \
  "pushq %%rax\n\t"                                                                                                    \
  "pushq %%rbx\n\t"                                                                                                    \
  "pushq %%rcx\n\t"                                                                                                    \
  "pushq %%rdx\n\t"                                                                                                    \
  "pushq %%rsi\n\t"                                                                                                    \
  "pushq %%rdi\n\t"                                                                                                    \
  "pushq %%rbp\n\t"                                                                                                    \
  "pushq %%r8\n\t"                                                                                                     \
  "pushq %%r9\n\t"                                                                                                     \
  "pushq %%r10\n\t"                                                                                                    \
  "pushq %%r11\n\t"                                                                                                    \
  "pushq %%r12\n\t"                                                                                                    \
  "pushq %%r13\n\t"                                                                                                    \
  "pushq %%r14\n\t"                                                                                                    \
  "pushq %%r15\n\t"

#define ASM_POP_ALL_REGISTERS                                                                                          \
  "popq %%r15\n\t"                                                                                                     \
  "popq %%r14\n\t"                                                                                                     \
  "popq %%r13\n\t"                                                                                                     \
  "popq %%r12\n\t"                                                                                                     \
  "popq %%r11\n\t"                                                                                                     \
  "popq %%r10\n\t"                                                                                                     \
  "popq %%r9\n\t"                                                                                                      \
  "popq %%r8\n\t"                                                                                                      \
  "popq %%rbp\n\t"                                                                                                     \
  "popq %%rdi\n\t"                                                                                                     \
  "popq %%rsi\n\t"                                                                                                     \
  "popq %%rdx\n\t"                                                                                                     \
  "popq %%rcx\n\t"                                                                                                     \
  "popq %%rbx\n\t"                                                                                                     \
  "popq %%rax\n\t"

namespace kernel::hw::interrupts {
[[gnu::naked, gnu::used]] void legacy_isr_common() noexcept {
  asm volatile(ASM_PUSH_ALL_REGISTERS

               "movq %%rsp, %%rdi\n\t"
               "call %P0\n\t"

               ASM_POP_ALL_REGISTERS

               "addq $16, %%rsp\n\t"
               "iretq\n\t"
               : /* No Output */
               : "s"(&dispatch_legacy)
               : "memory");
}

[[gnu::naked, gnu::used]] void legacy_nmi_common() noexcept {
  asm volatile(ASM_PUSH_ALL_REGISTERS

               "movq %%rsp, %%rdi\n\t"
               "call %P0\n\t"

               ASM_POP_ALL_REGISTERS

               "addq $16, %%rsp\n\t"
               "iretq\n\t"
               : /* No Output */
               : "s"(&dispatch_legacy_nmi)
               : "memory");
}

void fred_entrypoint() noexcept {
  asm volatile(".balign 4096\n\t"
               "pushq $0\n\t"

               ASM_PUSH_ALL_REGISTERS

               "movq %%rsp, %%rdi\n\t"
               "call %P0\n\t"

               ASM_POP_ALL_REGISTERS

               "addq $8, %%rsp\n\t"
               "testb $3, 16(%%rsp)\n\t" // Inspect CsInfo
               "jnz 1f\n\t"
               "erets\n\t" // CPL == 0, Return to Supervisor Mode
               "1:\n\t"
               "eretu\n\t" // CPL == 3, Return to User Mode
               :           /* No Output */
               : "s"(&dispatch_fred)
               : "memory");
}

namespace {
template <std::size_t Vector>
  requires(Vector == std::to_underlying(InterruptVector::NonMaskableInterrupt))
[[gnu::naked, gnu::used]] void legacy_stub() noexcept {
  asm volatile("pushq $0\n\t"
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
  asm volatile("pushq $0\n\t"
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
  asm volatile("pushq %0\n\t"
               "jmp %P1\n\t"
               : /* No Output */
               : "i"(Vector), "s"(&legacy_isr_common)
               : "memory");
}

template <std::size_t... Is> consteval auto generate_legacy_stubs(std::index_sequence<Is...>) {
  return std::array<StubPtr, IDT_SIZE>{legacy_stub<Is>...};
}

[[nodiscard]] Event translate_event(LegacyFrame *frame) noexcept {
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

  // Syscalls are handled by a separate branch
  return event;
}

[[nodiscard]] Event translate_event(FredFrame *frame) noexcept {
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

  switch (frame->event_info.get_type()) {
  case fred::EventType::ExternalInterrupt:
    event.category = EventClass::Interrupt;
    break;
  case fred::EventType::Nmi:
    event.category = EventClass::NMI;
    break;
  case fred::EventType::HardwareException:
  case fred::EventType::SoftwareException:
  case fred::EventType::PrivSoftwareException:
    event.category = EventClass::Exception;
    break;
  case fred::EventType::OtherEvent:        // syscall / sysenter
  case fred::EventType::SoftwareInterrupt: // INT n
    event.category = EventClass::Syscall;
    break;
  default:
    event.category = EventClass::Unknown;
    break;
  }

  return event;
}
} // namespace

std::array<StubPtr, IDT_SIZE> legacy_idt_stubs = generate_legacy_stubs(std::make_index_sequence<IDT_SIZE>{});

void dispatch_legacy(LegacyFrame *frame) noexcept {
  const auto event = translate_event(frame);
  InterruptManager::route_event(event);
}

void dispatch_legacy_nmi(LegacyFrame *frame) noexcept {
  const auto event = translate_event(frame);
  InterruptManager::route_event(event);
}

void dispatch_fred(FredFrame *frame) noexcept {
  const auto event = translate_event(frame);
  InterruptManager::route_event(event);
}
} // namespace kernel::hw::interrupts