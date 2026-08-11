#include "string.h"
#include <cstddef>
#include <cstdint>

namespace klib {
char *strtok_r(char *__restrict str, const char *__restrict delim,
               char **__restrict saveptr) noexcept {
  if (str == nullptr) {
    str = *saveptr;
  }

  if (str == nullptr) [[unlikely]] {
    return nullptr;
  }

  str += strspn(str, delim);
  if (*str == '\0') {
    *saveptr = nullptr;
    return nullptr;
  }

  // We have found the start of a valid token
  char *token = str;
  str = strpbrk(token, delim);

  if (str != nullptr) {
    *str = '\0';
    *saveptr = str + 1;
  } else {
    *saveptr = nullptr;
  }

  return token;
}

char *strtok(char *__restrict str, const char *__restrict delim) noexcept {
  static char *saved_ptr = nullptr;
  return strtok_r(str, delim, &saved_ptr);
}
} // namespace klib
