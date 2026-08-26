#!/usr/bin/env python3
"""Generate deterministic allocator-shaped MemX v1 traces.

The generator models granule/span metadata, not object payload. Small-object
activity therefore emits lookups inside a slab range while the index handle
identifies the slab metadata shared by those objects.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import random
import sys
from typing import Sequence, TextIO


INVALID_HANDLE = (1 << 64) - 1


@dataclasses.dataclass
class Allocation:
    base: int
    size: int
    handle: int
    owner_thread: int
    retired: bool = False


@dataclasses.dataclass(frozen=True)
class Event:
    sequence: int
    thread: int
    operation: str
    address: int
    size: int
    handle: int

    def write(self, output: TextIO) -> None:
        output.write(
            f"{self.sequence},{self.thread},{self.operation},"
            f"0x{self.address:x},{self.size},0x{self.handle:x}\n"
        )


class TraceGenerator:
    def __init__(
        self,
        *,
        seed: int,
        threads: int,
        granule_shift: int,
        region_shift: int,
        base: int,
        region_count: int,
    ) -> None:
        self.random = random.Random(seed)
        self.threads = threads
        self.granule_shift = granule_shift
        self.region_shift = region_shift
        self.granule_size = 1 << granule_shift
        self.region_size = 1 << region_shift
        self.base = base
        self.limit = base + region_count * self.region_size
        self.sequence = 0
        self.next_handle = 1
        self.allocations: list[Allocation] = []
        self.free_ranges: list[tuple[int, int]] = [(base, self.limit - base)]
        self.events: list[Event] = []

    def emit(
        self,
        thread: int,
        operation: str,
        address: int,
        size: int,
        handle: int,
    ) -> None:
        self.sequence += 1
        self.events.append(
            Event(self.sequence, thread, operation, address, size, handle)
        )

    def aligned_size(self, requested: int) -> int:
        return (requested + self.granule_size - 1) & -self.granule_size

    def allocate_range(self, size: int) -> int | None:
        candidates = [
            index for index, (_, available) in enumerate(self.free_ranges)
            if available >= size
        ]
        if not candidates:
            return None
        index = self.random.choice(candidates)
        base, available = self.free_ranges[index]
        # Occasionally place at the high end to create address-space gaps.
        if self.random.random() < 0.25:
            result = base + available - size
            remaining = available - size
            if remaining:
                self.free_ranges[index] = (base, remaining)
            else:
                self.free_ranges.pop(index)
        else:
            result = base
            remaining = available - size
            if remaining:
                self.free_ranges[index] = (base + size, remaining)
            else:
                self.free_ranges.pop(index)
        return result

    def free_range(self, base: int, size: int) -> None:
        self.free_ranges.append((base, size))
        self.free_ranges.sort()
        merged: list[tuple[int, int]] = []
        for current_base, current_size in self.free_ranges:
            if merged and merged[-1][0] + merged[-1][1] == current_base:
                previous_base, previous_size = merged[-1]
                merged[-1] = (previous_base, previous_size + current_size)
            else:
                merged.append((current_base, current_size))
        self.free_ranges = merged

    def insert(self) -> None:
        # Bias toward slab/span-like powers of two with occasional large runs.
        granules = self.random.choices(
            [1, 2, 4, 8, 16, 32, 64, 128, 256, 512],
            weights=[20, 18, 16, 14, 10, 8, 6, 4, 3, 1],
        )[0]
        size = granules * self.granule_size
        address = self.allocate_range(size)
        if address is None:
            return
        thread = self.random.randrange(self.threads)
        handle = self.next_handle
        self.next_handle += 1
        if handle == INVALID_HANDLE:
            self.next_handle += 1
        allocation = Allocation(address, size, handle, thread)
        self.allocations.append(allocation)
        self.emit(thread, "INSERT", address, size, handle)

    def lookup(self) -> None:
        active = [allocation for allocation in self.allocations if not allocation.retired]
        if not active:
            return
        allocation = self.random.choice(active)
        thread = (
            allocation.owner_thread
            if self.random.random() < 0.75
            else self.random.randrange(self.threads)
        )
        address = allocation.base + self.random.randrange(allocation.size)
        self.emit(thread, "LOOKUP", address, 0, allocation.handle)

    def retire(self) -> None:
        active = [allocation for allocation in self.allocations if not allocation.retired]
        if not active:
            return
        allocation = self.random.choice(active)
        allocation.retired = True
        self.emit(
            allocation.owner_thread,
            "RETIRE",
            allocation.base,
            allocation.size,
            allocation.handle,
        )

    def quiescent(self) -> None:
        thread = self.random.randrange(self.threads)
        self.emit(thread, "QUIESCENT", 0, 0, 0)

    def remove_retired(self) -> None:
        retired = [allocation for allocation in self.allocations if allocation.retired]
        if not retired:
            return
        allocation = self.random.choice(retired)
        self.emit(
            allocation.owner_thread,
            "REMOVE",
            allocation.base,
            allocation.size,
            allocation.handle,
        )
        self.allocations.remove(allocation)
        self.free_range(allocation.base, allocation.size)

    def step(self) -> None:
        roll = self.random.random()
        if roll < 0.16:
            self.insert()
        elif roll < 0.81:
            self.lookup()
        elif roll < 0.88:
            self.retire()
        elif roll < 0.94:
            self.quiescent()
        else:
            self.remove_retired()

    def finish(self) -> None:
        for thread in range(self.threads):
            self.emit(thread, "QUIESCENT", 0, 0, 0)
        for allocation in list(self.allocations):
            if not allocation.retired:
                allocation.retired = True
                self.emit(
                    allocation.owner_thread,
                    "RETIRE",
                    allocation.base,
                    allocation.size,
                    allocation.handle,
                )
        for thread in range(self.threads):
            self.emit(thread, "QUIESCENT", 0, 0, 0)
        for allocation in list(self.allocations):
            self.emit(
                allocation.owner_thread,
                "REMOVE",
                allocation.base,
                allocation.size,
                allocation.handle,
            )
            self.free_range(allocation.base, allocation.size)
            self.allocations.remove(allocation)


def arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--events", type=int, default=100_000)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--regions", type=int, default=256)
    parser.add_argument("--region-shift", type=int, default=21)
    parser.add_argument("--granule-shift", type=int, default=12)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x100000000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x6D656D78)
    parsed = parser.parse_args(argv)
    if parsed.events <= 0 or parsed.threads <= 0 or parsed.regions <= 0:
        parser.error("events, threads, and regions must be positive")
    if not 0 <= parsed.granule_shift <= parsed.region_shift < 63:
        parser.error("shifts must satisfy 0 <= granule <= region < 63")
    region_size = 1 << parsed.region_shift
    if parsed.base % region_size:
        parser.error("base must be region aligned")
    return parsed


def write_trace(path: pathlib.Path, generator: TraceGenerator) -> None:
    with path.open("x", encoding="utf-8") as output:
        output.write("MEMX_TRACE\n")
        output.write("version=1\n")
        output.write(f"region_shift={generator.region_shift}\n")
        output.write(f"granule_shift={generator.granule_shift}\n")
        output.write("sequence,thread,operation,address,size,handle\n")
        for generated_event in generator.events:
            generated_event.write(output)


def main(argv: Sequence[str] | None = None) -> int:
    parsed = arguments(sys.argv[1:] if argv is None else argv)
    generator = TraceGenerator(
        seed=parsed.seed,
        threads=parsed.threads,
        granule_shift=parsed.granule_shift,
        region_shift=parsed.region_shift,
        base=parsed.base,
        region_count=parsed.regions,
    )
    for _ in range(parsed.events):
        generator.step()
    generator.finish()
    try:
        write_trace(parsed.output, generator)
    except OSError as error:
        print(f"cannot write {parsed.output}: {error}", file=sys.stderr)
        return 1
    print(f"wrote {len(generator.events)} events to {parsed.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

