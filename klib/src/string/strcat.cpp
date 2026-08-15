#include "string.h"
#include <cstddef>

namespace klib {
char *strcat(char *__restrict dest, const char *__restrict src) noexcept {
  const std::size_t dest_len = strlen(dest);
  const std::size_t src_len = strlen(src);

  memcpy(dest + dest_len, src, src_len + 1);
  return dest;
}

char *strncat(char *__restrict dest, const char *__restrict src, size_t count) noexcept {
  if (count == 0) [[unlikely]] {
    return dest;
  }

  const std::size_t dest_len = strlen(dest);
  const std::size_t src_len = strnlen(src, count);
  memcpy(dest + dest_len, src, src_len);
  dest[dest_len + src_len] = '\0';

  return dest;
}
} // namespace klib
