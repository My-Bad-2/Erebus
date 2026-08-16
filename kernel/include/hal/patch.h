#pragma once

#define EREBUS_STATIC_BRANCH_UNLIKELY(key_obj)                                                                         \
  ({                                                                                                                   \
    __label__ l_yes, l_no;                                                                                             \
    bool _branch_res;                                                                                                  \
    asm goto("1:\n\t"                                                                                                  \
             "  .byte 0x0F, 0x1F, 0x44, 0x00, 0x00\n\t"                                                                \
             ".pushsection .static_keys,\"a\"\n\t"                                                                     \
             "  .long 1b - .\n\t"                                                                                      \
             "  .long %l[l_yes] - .\n\t"                                                                               \
             "  .long %P[key] - .\n\t"                                                                                 \
             ".popsection\n\t"                                                                                         \
             : /* No outputs */                                                                                        \
             : [key] "i"(&(key_obj))                                                                                   \
             : /* No clobbers */                                                                                       \
             : l_yes);                                                                                                 \
    _branch_res = false;                                                                                               \
    goto l_no;                                                                                                         \
  l_yes:                                                                                                               \
    _branch_res = true;                                                                                                \
  l_no:                                                                                                                \
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
               : [orig] "X"(fallback), [repl] "X"(opt), [feat] "X"(static_cast<std::uint32_t>(hw_feat))                \
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
               : [feat] "X"(static_cast<std::uint32_t>(hw_feat))                                                       \
               : "memory")

#define EREBUS_ALT_INLINE(old_asm, new_asm, hw_feat, ...)                                                              \
  EREBUS_ALT_INLINE_PRIO(old_asm, new_asm, hw_feat, 0, ##__VA_ARGS__)