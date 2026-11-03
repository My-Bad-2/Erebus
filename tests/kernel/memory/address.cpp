#include "memory/address.hpp"

#include <kformat.hpp>
#include <string_view>

#include "ktest.hpp"

using kernel::memory::PhysicalAddress;
using kernel::memory::VirtualAddress;

KTEST(Address, physical_arithmetic_and_alignment) {
  const PhysicalAddress p{0x1234};
  KEXPECT_EQ((p + 0x10).value(), 0x1244u);
  KEXPECT_EQ((p - 0x34).value(), 0x1200u);
  KEXPECT_EQ(PhysicalAddress{0x3000} - PhysicalAddress{0x1000}, 0x2000);
  KEXPECT_EQ(p.align_down(0x1000).value(), 0x1000u);
  KEXPECT_EQ(p.align_up(0x1000).value(), 0x2000u);
  KEXPECT(!(p.is_aligned(0x1000)));
  KEXPECT_LT(PhysicalAddress{1}, PhysicalAddress{2});
}

KTEST(Address, virtual_page_table_indices) {
  // PML4 511, PDPT 510, PD 1, PT 2, offset 0x123.
  const VirtualAddress v{0xFFFF'FFFF'8020'2123};
  KEXPECT(v.is_upper_half());
  KEXPECT_EQ(v.pml4_index(), 511u);
  KEXPECT_EQ(v.pml3_index(), 510u);
  KEXPECT_EQ(v.pml2_index(), 1u);
  KEXPECT_EQ(v.pml1_index(), 2u);
  KEXPECT_EQ(v.page_offset(), 0x123u);
}

KTEST(Address, virtual_basics) {
  KEXPECT(VirtualAddress{}.is_null());
  KEXPECT(!(static_cast<bool>(VirtualAddress{nullptr})));
  KEXPECT_EQ(VirtualAddress{0x1FFF}.align_up(0x1000).value(), 0x2000u);
  KEXPECT_EQ(VirtualAddress{0x1FFF}.align_down(0x1000).value(), 0x1000u);

  VirtualAddress v{0x1000};
  v += 0x20;
  KEXPECT_EQ(v.value(), 0x1020u);
  KEXPECT_EQ(VirtualAddress{0x2000} - VirtualAddress{0x1000}, 0x1000);
}

KTEST(Address, formatting) {
  char buf[64];
  klib::kprint(buf, sizeof(buf), "{} {}", PhysicalAddress{0xABC}, VirtualAddress{0x1000});
  const std::string_view text{buf};
  KEXPECT_NE(text.find("abc"), std::string_view::npos);
  KEXPECT_NE(text.find("1000"), std::string_view::npos);
}
