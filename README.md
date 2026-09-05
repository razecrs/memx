# Low-level libraries

This workspace contains independently scoped library projects.

- [MemX](memx/README.md): address metadata indexing and the experimental heap.
- [Latest MemX improvement measurements](memx/bench/results/large-index-2026-09-05.json)
  and [historical multi-platform comparison](memx/multi-test-benh.json).
- [Discord stack roadmap](foundations/ROADMAP.md): the planned runtime,
  networking primitives, and Discord protocol library, with comparison targets.

MemX ambition: beat every competitor under every condition we test. Measured
claims remain specific to workload, hardware, API contract, and memory cost.
MemX is the active project. The Discord runtime/protocol stack remains future
scope until the MemX milestone is completed.

Build MemX from this directory:

```sh
cmake -S memx -B memx/build-current -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build memx/build-current
ctest --test-dir memx/build-current --output-on-failure
```

The Git history and remote remain at the workspace root. Existing MemX source,
documentation, results, local research checkouts, and build artifacts were
moved into `memx/`. Historical reports retain their original paths as provenance.
Old CMake caches contain absolute paths from before the move; use a fresh build
directory instead of reusing those caches. External SDK/toolchain installations
have not moved.

The experimental MemX heap currently uses libc and pthreads. Integrating it into
a libc-free runtime requires a separately specified OS, locking, startup, and
metadata-allocation backend.
