#include "hal/gdt.hpp"

namespace kernel::hw::gdt {
std::array<GdtEntry, 2> GlobalDescriptorTable::make_tss_descriptors(const TaskStateSegment &tss) noexcept {
  const auto base = std::bit_cast<std::uintptr_t>(&tss);
  constexpr std::size_t limit = sizeof(TaskStateSegment) - 1;

  constexpr std::uint64_t TypeAvailable64 = 0x9ul << 40;
  constexpr std::uint64_t PresentFlag = 1ul << 47;

  std::uint64_t low = 0;
  low |= limit & 0xffff;
  low |= (base & 0xffff) << 16;
  low |= (base & 0xff0000) << 16;
  low |= TypeAvailable64 | PresentFlag;
  low |= (limit & 0xf0000) << 32;
  low |= (base & 0xff000000) << 32;

  const std::uint64_t high = base >> 32;

  return {GdtEntry(low), GdtEntry(high)};
}

void GlobalDescriptorTable::load() noexcept {
  const auto [tss_low, tss_high] = make_tss_descriptors(m_tss);
  m_entries[5] = tss_low;
  m_entries[6] = tss_high;

  const GdtDescriptor gdtr{
      .size = static_cast<std::uint16_t>(sizeof(m_entries) - 1),
      .offset = std::bit_cast<std::uint64_t>(m_entries.data()),
  };

  asm volatile("lgdt %0" : /* No Output */ : "m"(gdtr) : "memory");

  // Reset Data segments
  asm volatile("mov %0, %%ds \n\t"
               "mov %0, %%es \n\t"
               "mov %0, %%ss \n\t"
               "xor %%eax, %%eax \n\t"
               "mov %%eax, %%fs \n\t"
               "mov %%eax, %%gs \n\t"
               : /* No Output */
               : "r"(std::to_underlying(Selector::KernelData))
               : "memory");

  // Far jump to flush the Code Segment (CS) cache
  asm volatile("pushq %0\n\t"
               "lea 1f(%%rip), %%rax\n\t"
               "pushq %%rax\n\t"
               "lretq \n\t"
               "1: \n\t"
               : /* No Output */
               : "i"(std::to_underlying(Selector::KernelCode))
               : "rax", "memory");

  asm volatile("ltr %0" : : "r"(std::to_underlying(Selector::Tss)) : "memory");
}
} // namespace kernel::hw::gdt