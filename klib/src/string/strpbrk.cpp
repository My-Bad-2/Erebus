#include <cstddef>

#include "string.h"

namespace klib {
char *strpbrk(const char *dest, const char *src) noexcept {
  const std::size_t span = strcspn(dest, src);

  if (dest[span] == '\0') {
    return nullptr;
  }

  return const_cast<char *>(dest + span);
}

char *strstr(const char *haystack, const char *needle) noexcept {
  const std::size_t needle_len = strlen(needle);

  if (needle_len == 0) [[unlikely]] {
    return const_cast<char *>(haystack);
  }

  const std::size_t haystack_len = strlen(haystack);

  // Impossible to find a substring longer than the haystack.
  if (haystack_len < needle_len) [[unlikely]] {
    return nullptr;
  }

  // We only need to search up to the point where the remaining haystack
  // is exactly the size of the needle.
  const char *current = haystack;
  const std::size_t search_bounds = haystack_len - needle_len + 1;
  const char *end_search = haystack + search_bounds;

  const char first_char = needle[0];
  const char last_char = needle[needle_len - 1];

  while (current < end_search) {
    // Jump directly to the next possible match
    current = static_cast<const char *>(
        memchr(current, first_char, end_search - current));

    if (!current) {
      // First character no longer exists in search bounds
      return nullptr;
    }

    // We check the LAST character of the needle. This check defeats the
    // worst-case scenarios where the haystack has heavily repeating prefixes
    // (e.g. "AAAAAB").
    if (current[needle_len - 1] == last_char) {
      // If the needle is 1 or 2 chars, the first & last checks already proved
      // it! Otherwise, blast the remaining middle bytes with memcmp.
      if (needle_len <= 2 ||
          memcmp(current + 1, needle + 1, needle_len - 2) == 0) {
        return const_cast<char *>(current);
      }
    }

    ++current;
  }

  return nullptr;
}
} // namespace klib
