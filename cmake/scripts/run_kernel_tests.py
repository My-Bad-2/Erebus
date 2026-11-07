#!/usr/bin/env python3
"""Boots the test kernel in QEMU, once or across a matrix of machine configurations.

Every combination of accelerator, CPU model, CPU count, memory size and NUMA node count is
booted --iterations times. A boot passes once every --expect pattern has shown up on the serial
console, and fails as soon as a --fail pattern does, if QEMU exits first, or if it times out.
Serial logs of every boot are kept in --log-dir; --echo also copies the console to the terminal
while it runs, which is meant for a single boot. The exit status is 0 only if every boot passed.

Normally run through the check and qemu_stress targets, which fill in the arguments from the
EREBUS_QEMU_* cache variables.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import itertools
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[=>][0-9]*[A-Za-z]?")
POLL_INTERVAL = 0.2


@dataclasses.dataclass(frozen=True)
class Config:
    accel: str
    cpu: str
    smp: int
    memory_mib: int
    numa_nodes: int
    iteration: int
    topology: str = ""  # What goes after the CPU count in -smp, like sockets=2,cores=2.

    @property
    def name(self) -> str:
        cpu = re.sub(r"[^A-Za-z0-9]+", "_", self.cpu).strip("_")
        shape = re.sub(r"[^A-Za-z0-9]+", "_", self.topology)
        return (f"{self.accel}-{cpu}-smp{self.smp}{shape}-{self.memory_mib}M"
                f"-numa{self.numa_nodes}-run{self.iteration}")

    @property
    def summary(self) -> str:
        return (f"{self.accel:<4} {self.cpu:<45} smp={self.smp_argument:<3} mem={self.memory_mib}M "
                f"numa={self.numa_nodes} #{self.iteration}")


    @property
    def smp_argument(self) -> str:
        return f"{self.smp},{self.topology}" if self.topology else str(self.smp)


@dataclasses.dataclass
class Result:
    config: Config
    passed: bool
    seconds: float
    reason: str
    log: Path


def parse_memory(text: str) -> int:
    """'512M', '2G' or a plain number of MiB, in MiB."""
    match = re.fullmatch(r"\s*(\d+)\s*([MmGg]?)\s*", text)
    if not match:
        raise argparse.ArgumentTypeError(f"not a memory size: {text!r}")
    value = int(match.group(1))
    return value * 1024 if match.group(2).lower() == "g" else value


def kvm_usable() -> bool:
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def numa_arguments(config: Config) -> list[str]:
    """Splits the memory and the CPUs over the nodes, as evenly as they go."""
    if config.numa_nodes <= 1:
        return []

    args: list[str] = []
    per_node_mib = config.memory_mib // config.numa_nodes
    cpus_per_node = config.smp // config.numa_nodes
    first_cpu = 0
    for node in range(config.numa_nodes):
        last_node = node == config.numa_nodes - 1
        size = config.memory_mib - per_node_mib * node if last_node else per_node_mib
        last_cpu = config.smp - 1 if last_node else first_cpu + cpus_per_node - 1
        args += ["-object", f"memory-backend-ram,id=mem{node},size={size}M",
                 "-numa", f"node,nodeid={node},cpus={first_cpu}-{last_cpu},memdev=mem{node}"]
        first_cpu = last_cpu + 1
    return args


def qemu_command(args: argparse.Namespace, config: Config, log: Path, vars_file: Path) -> list[str]:
    return [
        args.qemu,
        "-M", "q35",
        "-m", f"{config.memory_mib}M",
        "-smp", config.smp_argument,
        "-cpu", config.cpu,
        "-accel", config.accel,
        *numa_arguments(config),
        "-cdrom", str(args.iso),
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code}",
        "-drive", f"if=pflash,format=raw,readonly=off,file={vars_file}",
        "-display", "none",
        "-serial", f"file:{log}",
        "-no-reboot",
        "-no-shutdown",
        *args.extra_arg,
    ]


def run_one(args: argparse.Namespace, config: Config) -> Result:
    log = args.log_dir / f"{config.name}.log"
    vars_file = args.log_dir / f"{config.name}.vars.fd"
    # Every guest needs firmware variables of its own to write to.
    shutil.copyfile(args.ovmf_vars, vars_file)
    log.write_bytes(b"")

    expect = [re.compile(pattern) for pattern in args.expect]
    fail = [re.compile(pattern) for pattern in args.fail]

    start = time.monotonic()
    process = subprocess.Popen(qemu_command(args, config, log, vars_file),
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)

    def finish(passed: bool, reason: str) -> Result:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        vars_file.unlink(missing_ok=True)
        return Result(config, passed, time.monotonic() - start, reason, log)

    echoed = 0
    while True:
        raw = log.read_text(errors="replace")
        if args.echo and len(raw) > echoed:
            sys.stdout.write(raw[echoed:])
            sys.stdout.flush()
            echoed = len(raw)
        text = ANSI_ESCAPE.sub("", raw).replace("\r", "")

        for pattern in fail:
            if match := pattern.search(text):
                line = text[text.rfind("\n", 0, match.start()) + 1:].split("\n", 1)[0]
                return finish(False, f"failure on the console: {line.strip()}")

        if all(pattern.search(text) for pattern in expect):
            return finish(True, "")

        if process.poll() is not None:
            stderr = process.stderr.read().decode(errors="replace").strip() if process.stderr else ""
            detail = f": {stderr.splitlines()[-1]}" if stderr else ""
            return finish(False, f"QEMU exited with status {process.returncode} before the "
                                 f"kernel finished{detail}")

        if time.monotonic() - start > args.timeout:
            lines = [line for line in text.splitlines() if line.strip()]
            last = lines[-1].strip() if lines else "no output at all"
            return finish(False, f"timed out after {args.timeout} s; last line: {last}")

        time.sleep(POLL_INTERVAL)


def build_matrix(args: argparse.Namespace) -> tuple[list[Config], list[str]]:
    configs: list[Config] = []
    skipped: list[str] = []
    accels = list(args.accel)
    if "kvm" in accels and not kvm_usable():
        accels.remove("kvm")
        skipped.append("kvm: /dev/kvm is not usable by this user")

    for accel, cpu, machine, memory, nodes in itertools.product(
            accels, args.cpu, args.smp, args.memory, args.numa):
        smp, _, topology = machine.partition(",")
        if nodes > int(smp):
            continue  # A node without CPUs is legal, but not what anyone means by this.
        for iteration in range(1, args.iterations + 1):
            configs.append(Config(accel, cpu, int(smp), memory, nodes, iteration, topology))
    return configs, skipped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--ovmf-code", type=Path, required=True)
    parser.add_argument("--ovmf-vars", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path, required=True)
    # Each of these is given once per value; the defaults apply when one is not given at all.
    parser.add_argument("--accel", action="append", help="accelerator (default: kvm)")
    parser.add_argument("--cpu", action="append", help="CPU model (default: max,migratable=no)")
    parser.add_argument("--smp", action="append",
                        help="CPU count, and optionally a shape as QEMU's -smp takes it, like "
                             "8,sockets=2,cores=2,threads=2 (default: 1, 2, 4)")
    parser.add_argument("--memory", action="append", type=parse_memory,
                        help="memory size such as 512M or 2G (default: 2G)")
    parser.add_argument("--numa", action="append", type=int, help="NUMA node count (default: 1)")
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds per boot")
    parser.add_argument("--jobs", type=int, default=1, help="boots to run at the same time")
    parser.add_argument("--expect", action="append", default=[],
                        help="regex that must appear on the console for a boot to pass")
    parser.add_argument("--fail", action="append", default=[],
                        help="regex that fails a boot as soon as it appears")
    parser.add_argument("--extra-arg", action="append", default=[],
                        help="extra QEMU argument, once per argument")
    parser.add_argument("--echo", action="store_true",
                        help="copy the serial console to the terminal as it comes")
    args = parser.parse_args()
    args.accel = args.accel or ["kvm"]
    args.cpu = args.cpu or ["max,migratable=no"]
    args.smp = args.smp or ["1", "2", "4"]
    args.memory = args.memory or [2048]
    args.numa = args.numa or [1]

    if not args.expect:
        parser.error("give at least one --expect pattern, or no boot can ever pass")
    for path in (args.iso, args.ovmf_code, args.ovmf_vars):
        if not path.is_file():
            parser.error(f"{path} does not exist")

    configs, skipped = build_matrix(args)
    for reason in skipped:
        print(f"skipped {reason}")
    if not configs:
        print("nothing to run")
        return 1

    args.log_dir.mkdir(parents=True, exist_ok=True)
    print(f"{len(configs)} boots, {args.jobs} at a time, logs in {args.log_dir}", flush=True)

    results: list[Result] = []
    print_lock = threading.Lock()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for future in concurrent.futures.as_completed(pool.submit(run_one, args, c) for c in configs):
            result = future.result()
            results.append(result)
            with print_lock:
                status = "PASS" if result.passed else "FAIL"
                print(f"[{len(results):>3}/{len(configs)}] {status} {result.config.summary} "
                      f"{result.seconds:6.1f} s", flush=True)
                if not result.passed:
                    print(f"          {result.reason}\n          log: {result.log}", flush=True)

    failures = [r for r in results if not r.passed]
    slowest = max(results, key=lambda r: r.seconds)
    print(f"\n{len(results) - len(failures)} of {len(results)} boots passed; slowest "
          f"{slowest.seconds:.1f} s ({slowest.config.summary.strip()})")
    for result in sorted(failures, key=lambda r: r.config.name):
        print(f"  FAIL {result.config.summary}: {result.reason}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
