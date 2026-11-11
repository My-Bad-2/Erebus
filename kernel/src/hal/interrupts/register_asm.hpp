// The general-purpose registers, saved and restored in inline assembly in the order of
// interrupts::RegisterContext: RAX is pushed first, so R15 ends up at the lowest address, where the
// struct starts. Every way into the kernel from outside it uses these, so that one RegisterContext
// describes them all.
#pragma once

#define ASM_PUSH_ALL_REGISTERS \
  "pushq %%rax\n\t"            \
  "pushq %%rbx\n\t"            \
  "pushq %%rcx\n\t"            \
  "pushq %%rdx\n\t"            \
  "pushq %%rsi\n\t"            \
  "pushq %%rdi\n\t"            \
  "pushq %%rbp\n\t"            \
  "pushq %%r8\n\t"             \
  "pushq %%r9\n\t"             \
  "pushq %%r10\n\t"            \
  "pushq %%r11\n\t"            \
  "pushq %%r12\n\t"            \
  "pushq %%r13\n\t"            \
  "pushq %%r14\n\t"            \
  "pushq %%r15\n\t"

#define ASM_POP_ALL_REGISTERS \
  "popq %%r15\n\t"            \
  "popq %%r14\n\t"            \
  "popq %%r13\n\t"            \
  "popq %%r12\n\t"            \
  "popq %%r11\n\t"            \
  "popq %%r10\n\t"            \
  "popq %%r9\n\t"             \
  "popq %%r8\n\t"             \
  "popq %%rbp\n\t"            \
  "popq %%rdi\n\t"            \
  "popq %%rsi\n\t"            \
  "popq %%rdx\n\t"            \
  "popq %%rcx\n\t"            \
  "popq %%rbx\n\t"            \
  "popq %%rax\n\t"
