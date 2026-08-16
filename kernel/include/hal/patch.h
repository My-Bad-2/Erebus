#pragma once

#define EREBUS_STATIC_BRANCH_UNLIKELY(key_obj)                                                                         \
  ({                                                                                                                   \
    bool _branch_res = false;                                                                                          \
    asm goto("1:\n\t"                                                                                                  \
             "  .byte 0x0F, 0x1F, 0x44, 0x00, 0x00\n\t"                                                                \
             ".pushsection .static_keys,\"a\"\n\t"                                                                     \
             "  .long 1b - .\n\t"                                                                                      \
             "  .long %l[l_yes] - .\n\t"                                                                               \
             "  .long %P[key] - .\n\t" /* Relative offset to the C++ bool */                                           \
             ".popsection\n\t"                                                                                         \
             :                       /* No outputs */                                                                  \
             : [key] "i"(&(key_obj)) /* Bind the C++ variable */                                                       \
             :                       /* No clobbers */                                                                 \
             : l_yes);                                                                                                 \
    if (0) {                                                                                                           \
    l_yes:                                                                                                             \
      _branch_res = true;                                                                                              \
    }                                                                                                                  \
    _branch_res;                                                                                                       \
  })

#define EREBUS_HOTSWAP_CALL_PRIO(fallback, opt, hw_feat, prio)                                                         \
  asm volatile("661:\n\t"                                                                                              \
               "  call %P[orig]\n\t"                                                                                   \
               "662:\n\t"                                                                                              \
               ".pushsection .altinstructions,\"a\"\n\t"                                                               \
               "  .long 661b - .\n\t"                                                                                  \
               "  .long 663f - .\n\t"                                                                                  \
               "  .long %c[feat]\n\t"                                                                                  \
               "  .byte 662b - 661b\n\t"                                                                               \
               "  .byte 664f - 663f\n\t"                                                                               \
               "  .byte " #prio "\n\t"                                                                                 \
               "  .byte 0\n\t"                                                                                         \
               ".popsection\n\t"                                                                                       \
               ".pushsection .altinstr_replacement,\"ax\"\n\t"                                                         \
               "663:\n\t"                                                                                              \
               "  call %P[repl]\n\t"                                                                                   \
               "664:\n\t"                                                                                              \
               ".popsection\n\t"                                                                                       \
               :                                                                                                       \
               : [orig] "i"(fallback), [repl] "i"(opt), [feat] "i"(static_cast<std::uint32_t>(hw_feat))                \
               : "memory")

#define EREBUS_HOTSWAP_CALL(fallback, opt, hw_feat) EREBUS_HOTSWAP_CALL_PRIO(fallback, opt, hw_feat, 0)

#define EREBUS_ALT_INLINE_PRIO(old_asm, new_asm, hw_feat, prio, ...)                                                   \
  asm volatile("661:\n\t"                                                                                              \
               "  " old_asm "\n\t"                                                                                     \
               "662:\n\t"                                                                                              \
               ".pushsection .altinstructions,\"a\"\n\t"                                                               \
               "  .long 661b - .\n\t"                                                                                  \
               "  .long 663f - .\n\t"                                                                                  \
               "  .long %c[feat]\n\t"                                                                                  \
               "  .byte 662b - 661b\n\t"                                                                               \
               "  .byte 664f - 663f\n\t"                                                                               \
               "  .byte " #prio "\n\t"                                                                                 \
               "  .byte 0\n\t"                                                                                         \
               ".popsection\n\t"                                                                                       \
               ".pushsection .altinstr_replacement,\"ax\"\n\t"                                                         \
               "663:\n\t"                                                                                              \
               "  " new_asm "\n\t"                                                                                     \
               "664:\n\t"                                                                                              \
               ".popsection\n\t"                                                                                       \
               : __VA_ARGS__                                                                                           \
               : [feat] "i"(static_cast<std::uint32_t>(hw_feat))                                                       \
               : "memory")

#define EREBUS_ALT_INLINE(old_asm, new_asm, hw_feat, ...)                                                              \
  EREBUS_ALT_INLINE_PRIO(old_asm, new_asm, hw_feat, 0, ##__VA_ARGS__)