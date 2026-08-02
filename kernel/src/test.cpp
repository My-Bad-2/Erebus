#include <stddef.h>
#include <string.h>

namespace {
	[[maybe_unused]] char test[4096];
}

extern "C" void _start() {
	klibc::memset(test, 0, sizeof(test));
	size_t i = 0;
	while (true)
		;
}
