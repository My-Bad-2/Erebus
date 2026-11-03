# Erebus

[![Kernel tests](https://github.com/My-Bad-2/Erebus/actions/workflows/tests.yml/badge.svg)](https://github.com/My-Bad-2/Erebus/actions/workflows/tests.yml)
[![Release](https://img.shields.io/github/v/release/My-Bad-2/Erebus?include_prereleases&sort=semver)](https://github.com/My-Bad-2/Erebus/releases)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE.md)
![C++](https://img.shields.io/badge/C%2B%2B-gnu%2B%2B2d-00599C)
![Platform](https://img.shields.io/badge/platform-x86--64%20UEFI-lightgrey)

Erebus is a hobby kernel for x86-64 PCs, written in freestanding C++ and booted by Limine.

Right now it boots, takes over memory, wakes up every CPU, calibrates its clocks, says
`Hello, World!` and then idles. Forever. There is no scheduler, no userspace and no filesystem.
Not yet, anyway.

The parts that do exist:

- A buddy allocator for physical memory, split per NUMA node and per mobility class, with
  per-CPU page caches in front of it. Blocks go from 4 KiB all the way up to 1 GiB.
- SLUB-style slab caches and a kernel heap on top of that. Yes, it follows Linux's SLUB closely.
  On purpose.
- Address spaces built out of VMAs: 4- or 5-level paging, PCID, memory filled in on the first
  fault, stacks that grow by themselves, and copy-on-write (Mooooo!).
- Every CPU online, on x2APIC or xAPIC, with an I/O APIC for device interrupts and cross-CPU
  function calls (IPIs).
- ACPI through uACPI. A monotonic clock on the invariant TSC, measured against the HPET or the
  ACPI PM timer when the CPU won't say how fast it runs.
- A random number generator: a BLAKE2b entropy pool feeding a ChaCha20 generator per CPU.
- klib, a small C library of its own (strings, stdlib, ctype) and a formatting library whose
  format strings are checked at compile time.

## Building it

You need:

- Clang and LLD, and a recent pair. CI uses LLVM 23; much older releases won't support `-std=gnu++2d`.
- CMake 3.28 or newer, and Ninja.
- xorriso, which packs the ISO.
- QEMU (`qemu-system-x86_64`) and Python 3, to run the kernel and its tests.
- Doxygen, only if you want the API docs. Graphviz makes its graphs nicer.

Everything else is fetched for you. The git submodules are checked out if you forgot
`--recursive`, and Limine and OVMF come as prebuilt downloads, checked against pinned SHA-256
hashes.

```sh
git clone --recursive https://github.com/My-Bad-2/Erebus.git
cd Erebus
cmake --preset debug
cmake --build --preset debug
```

That leaves you with `build-debug/erebus.iso`, the kernel, and `build-debug/erebus-test.iso`,
the same kernel with its tests built in. The `release` preset does the same thing in `build/`,
with -O3 and ThinLTO.

If the LLVM you want isn't the one on your `PATH`:

```sh
cmake --preset debug -DLLVM_PREFIX=/usr/lib/llvm-23
```

## Running it

```sh
cmake --build --preset debug --target qemu_run        # a QEMU window, serial console in the terminal
cmake --build --preset debug --target qemu_headless   # no window; Ctrl-A X quits
cmake --build --preset debug --target qemu_debug      # frozen at the first instruction, GDB stub on :1234
```

For `qemu_debug`, attach with `gdb build-debug/kernel/kernel -ex 'target remote :1234'`. KASLR
is off in `misc/limine.conf`, so the addresses in the ELF are the ones you get.

The guest gets 2 GiB of memory, 4 CPUs, `-cpu max` and KVM when the host has it. All of that is
in the cache:

```sh
cmake --preset debug -DEREBUS_QEMU_SMP=16 -DEREBUS_QEMU_MEMORY=8G
```

Other knobs are `EREBUS_QEMU_CPU`, `EREBUS_QEMU_ACCEL` (`auto`, `kvm`, `hvf`, `whpx`, `tcg`)
and `EREBUS_QEMU_EXTRA_ARGS`.

Live Testing? The ISO is a hybrid image, so writing it to a USB stick should boot on a UEFI
machine. Should is the keyword. No Guarantees.

## Testing

The tests don't run on your host, they run in the kernel itself. A second image, `kernel_test`, is
linked from the same objects as the real kernel plus everything under `tests/`. It boots in
QEMU, runs every test and reports on the serial console, and QEMU's exit status says how it
went: 1 if everything passed, 3 if something failed.

```sh
cmake --build --preset debug --target check   # one boot, console on screen
ctest --preset kernel                          # the same boot through CTest, after a build
cmake --workflow --preset check                # configure, build and test in one go
```

A run looks like this:

```text
ktest: running 196 tests on 4 CPUs
ktest: pass Maths.alignment (0 ms)
...
ktest: 196 passed, 0 failed, 0 skipped
ktest: ALL PASSED
```

Then there is the stress test, which boots the test kernel over and over: 1, 2, 4 and 8 CPUs,
512 MiB and 2 GiB, one NUMA node or two, and CPU models with x2APIC, PCID/INVPCID/FSGSBASE or
the hardware RNG taken away. Twice each. That's 112 boots.

```sh
cmake --preset release
cmake --build --preset release --target qemu_stress
```

`cmake --workflow --preset stress` does both. The matrix lives in the `EREBUS_QEMU_STRESS_*`
cache variables (`-DEREBUS_QEMU_STRESS_JOBS=4` boots four at a time, if you have the cores),
and every boot leaves its serial log in `build/qemu_stress/`.

### Writing a test

Put a `.cpp` file anywhere under `tests/`, and the build picks it up:

```cpp
#include "utils/maths.hpp"

#include "ktest.hpp"

namespace maths = kernel::utils::maths;

KTEST(Maths, alignment) {
  KEXPECT_EQ(maths::align_up(0x1234u, 0x1000u), 0x2000u);
  KEXPECT(maths::is_aligned(0x3000ull, 0x1000u));
}
```

`KEXPECT_*` reports a failure and carries on, and `KREQUIRE_*` reports it and leaves the test.
`KTEST_ALL_CPUS` runs the body on every CPU at the same time, and `KTEST_SKIP` gives up
politely when the machine can't run the test. The whole framework is described in
[tests/framework/ktest.hpp](tests/framework/ktest.hpp).

## Arch levels, CI and releases

By default, the kernel is built for `x86-64-v2`, which any x86-64 CPU since Intel Nehalem or AMD
Bulldozer can run. If you know your CPU, you can be pickier:

```sh
cmake --preset release -DEREBUS_ARCH_LEVEL=x86-64-v3   # or x86-64-v4, znver4, znver5
```

Every push runs the test kernel on GitHub Actions, in Debug and in Release. Releases are cut by
hand from the Release workflow: it builds one ISO for every arch level, boots each one's tests
wherever the runner's CPU can, and publishes the ISOs with their kernel ELFs and a
`SHA256SUMS`. The version is whatever `project()` in `CMakeLists.txt` says.

## Other things the build does

Each of these wants its configure preset run once first: `cmake --preset release` before the
docs, `cmake --preset lint` before the lint build.

| Command | What you get |
|---|---|
| `cmake --build --preset docs` | Doxygen HTML in `build/docs/html/`, and the whole API as JSON in `build/docs/api.json` |
| `cmake --build --preset lint` | A Debug build in `build-lint/`, with clang-tidy going over the code |
| `cmake --build --preset debug --target format` | Everything run through clang-format |
| `cmake --build --preset debug --target format_check` | The same check, without touching any file |

And two options worth knowing: `-DEREBUS_UBSAN=ON` makes undefined behaviour trap, and
`-DEREBUS_WERROR=ON` turns every warning into an error. By default only the warnings that are
always bugs stop the build.

## Where things are

```text
kernel/        the kernel: boot, hal, memory, drivers, crypto, patch, utils
klib/          the freestanding C library and kformat
tests/         the in-kernel test framework, and the tests
cmake/         build modules, the QEMU test runner, Doxygen templates
third_party/   submodules, and the recipes for the Limine and OVMF downloads
misc/          limine.conf
```

## License

Erebus is released under the BSD 3-Clause License; see [LICENSE.md](LICENSE.md).
Copyright 2026 Yash Verma.

## Third-party code

None of this would boot without other people's work. Thank you, all of you.

| Project | What Erebus uses it for  |
|---|---|---|
| [Limine](https://github.com/Limine-Bootloader/Limine), by Mintsuki and contributors | The bootloader. Its UEFI binaries go into every ISO, and its protocol header is `kernel/include/boot/limine.h`. |
| [uACPI](https://github.com/uACPI/uACPI), by Daniil Tatianin | ACPI. The kernel only answers its questions, in `kernel/src/glue/uacpi.cpp`. |
| [Zydis](https://github.com/zyantific/zydis) and [Zycore](https://github.com/zyantific/zycore-c), by Florian Bernd and Joel Höner | x86 instruction decoding and encoding, linked into the kernel | 
| [freestanding-cxx-hdrs-gnu](https://github.com/osdev0/freestanding-cxx-hdrs-gnu) | The freestanding part of GCC's libstdc++ headers: `<atomic>`, `<span>`, `<expected>` and friends | 
| [freestanding-lgrt](https://github.com/osdev0/freestanding-lgrt) | Compiler runtime builtins from LLVM's compiler-rt, such as 128-bit division and wide atomics |
| [edk2-ovmf-stable-bins](https://github.com/osdev0/edk2-ovmf-stable-bins), OVMF from [TianoCore EDK II](https://github.com/tianocore/edk2) | UEFI firmware for QEMU. It only runs the kernel and is never part of an image. |

Each submodule carries its own license file under `third_party/`, and Limine's comes with the
download.
