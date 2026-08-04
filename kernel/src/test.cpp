#include <kformat.hpp>
#include <stddef.h>

namespace {
	[[maybe_unused]] char test[4096];
}

extern "C" void _start() {
	klib::kprint(test, sizeof(test), "|{:>10}|", "Erebus");
	while (true)
		;
}
