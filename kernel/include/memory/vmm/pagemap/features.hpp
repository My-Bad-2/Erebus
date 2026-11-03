/// @file features.hpp
/// @brief Paging-related CPU features, detected once on the boot CPU.

#pragma once

#include <cstdint>

namespace kernel::memory::vmm {
/// @brief What the paging code may rely on. Filled in by early_initialize_hw() and treated as
///        read-only afterwards; every CPU is assumed to support the same set as the BSP.
struct PagingFeatures {
  bool nx{false};       ///< No-execute bit (EFER.NXE).
  bool pge{false};      ///< Global pages (CR4.PGE).
  bool pcid{false};     ///< Process-context identifiers (CR4.PCIDE).
  bool invpcid{false};  ///< The INVPCID instruction.
  bool invlpgb{false};  ///< AMD's broadcast INVLPGB/TLBSYNC.
  bool huge_1g{false};  ///< 1 GiB pages.
  bool smep{false};
  bool smap{false};
  bool umip{false};
  bool pke{false};
  bool tce{false};  ///< AMD translation cache extension.

  std::uint16_t invlpgb_max_count{0};  ///< Largest extra-page count one INVLPGB accepts.
  std::uint8_t levels{4};              ///< Paging depth: 4, or 5 with LA57.
};

inline PagingFeatures g_paging_features{};
}  // namespace kernel::memory::vmm
