# MemX

> **MemX** is an experimental, architecture-aware memory metadata indexing library focused on making `pointer -> allocation metadata` lookup as fast and compact as possible across many CPU architectures.

The core research question:

> **Can an adaptive address-region index approach flat-table lookup performance while using substantially less metadata memory than a flat page map or conventional radix-based structure?**

MemX is **not yet a finished algorithm**.

Right now, it is a research direction consisting of:

* a defined problem;
* a proposed metadata representation;
* an adaptive indexing strategy;
* a lifetime state model;
* portable C reference implementations;
* per-ISA optimized implementations;
* benchmarks against existing allocator metadata lookup techniques.

The goal is to turn those pieces into a precisely specified, benchmarkable algorithm.

---

# 1. Why MemX Exists

Memory allocators frequently need to answer a deceptively simple question:

```text
Given pointer P:

    What allocation / span / page / region does P belong to?

    What size class is it?

    Is the allocation active?

    Where is its allocator metadata?
```

Conceptually:

```text
pointer
   ↓
address-space index
   ↓
allocation metadata
```

This operation can exist on extremely hot paths such as:

```text
malloc()
free()
realloc()
ownership checks
size recovery
remote frees
allocator diagnostics
```

The fastest theoretical lookup is something close to a giant direct map:

```c
entry = table[address >> PAGE_SHIFT];
```

That is excellent for latency.

It is potentially horrible for memory usage.

The opposite extreme is a compact tree:

```text
address
   ↓
root
   ↓
node
   ↓
node
   ↓
metadata
```

That saves metadata space but introduces:

* pointer chasing;
* additional cache accesses;
* branches;
* potentially unpredictable memory loads.

MemX attempts to occupy the useful middle ground.

---

# 2. Existing Systems

MemX is **not based on the assumption that pointer metadata lookup is a new problem**.

Existing allocators already solve related problems.

Examples include:

## TCMalloc

TCMalloc uses a page map to recover span/allocation metadata from addresses.

Conceptually:

```text
pointer
   ↓
page number
   ↓
PageMap / radix structure
   ↓
Span
   ↓
size class / allocator metadata
```

---

## jemalloc

jemalloc uses an `rtree`-style address lookup structure for extent metadata.

Conceptually:

```text
pointer
   ↓
rtree
   ↓
extent metadata
```

---

## snmalloc

snmalloc uses address-space metadata structures and compact metadata entries.

It is particularly interesting because it aggressively considers:

* address layout;
* metadata packing;
* ownership;
* cross-thread deallocation.

---

## mimalloc

mimalloc often derives metadata through large aligned segments and pages.

Conceptually:

```text
pointer
   ↓
segment computation
   ↓
page
   ↓
metadata
```

This can avoid some forms of generic global pointer lookup.

---

# 3. MemX's Potential Distinction

If MemX merely implements:

```text
pointer -> metadata
```

then it would simply reproduce existing allocator machinery.

The potentially interesting idea is:

> **Use different metadata representations for different address-space regions depending on their structure.**

Instead of forcing every region through the same representation:

```text
Region A -> radix tree
Region B -> radix tree
Region C -> radix tree
Region D -> radix tree
```

MemX could permit:

```text
Region A -> UNIFORM
Region B -> DENSE
Region C -> SPARSE
Region D -> RUN
```

The representation adapts to the structure of that region.

---

# 4. High-Level Lookup Model

A pointer is divided conceptually into:

```text
+----------------------+----------------------+
|     region index     |    region offset     |
+----------------------+----------------------+
```

For example:

```c
region = address >> MEMX_REGION_SHIFT;
offset = address & MEMX_REGION_MASK;
```

The region index locates a compact descriptor:

```text
pointer
   ↓
region index
   ↓
region descriptor
   ↓
descriptor-specific lookup
   ↓
allocation metadata
```

The descriptor determines the lookup method.

---

# 5. Proposed Region Types

Initial representation types:

```text
UNIFORM
DENSE
SPARSE
RUN
```

These names are provisional.

---

## 5.1 UNIFORM

Every address unit within the region resolves to the same metadata.

Example:

```text
Region
+-----------------------------------------------+
| A | A | A | A | A | A | A | A | A | A | A |
+-----------------------------------------------+
```

Instead of storing eleven entries:

```text
A A A A A A A A A A A
```

store:

```text
UNIFORM -> A
```

Lookup:

```c
descriptor = regions[region];

if (descriptor.type == MEMX_UNIFORM)
{
    return descriptor.metadata;
}
```

This is potentially extremely fast:

```text
index
load
tag test
return
```

---

# 5.2 DENSE

A region contains enough variation that a direct local table makes sense.

```text
Region
+-----------------------------------------------+
| A | A | B | C | C | D | D | E | F | F | G |
+-----------------------------------------------+
```

Lookup:

```c
entry_index =
    (address >> MEMX_GRANULE_SHIFT) & MEMX_LOCAL_MASK;

return descriptor.table[entry_index];
```

This sacrifices memory for speed.

---

# 5.3 SPARSE

A region contains mostly nothing or mostly a default value with a small number of exceptions.

Example:

```text
default = EMPTY

exceptions:

page 7  -> A
page 39 -> B
page 40 -> B
```

Rather than allocating a full dense table:

```text
SPARSE
    default
    exception map
```

Possible implementations require experimentation:

* small sorted arrays;
* bitmaps;
* compact index arrays;
* tiny hash structures;
* branchless bitmap ranking;
* hybrid representations.

No implementation should be selected before benchmarking.

---

# 5.4 RUN

Metadata appears in contiguous runs.

Example:

```text
pages  0-15  -> A
pages 16-31  -> B
pages 32-63  -> C
```

Represent as:

```text
[0,16)  -> A
[16,32) -> B
[32,64) -> C
```

Potential structures:

```text
run boundaries
run descriptor array
small interval lookup
```

This might work well for allocator layouts where spans naturally cover contiguous page ranges.

---

# 6. Adaptive Representation

MemX could dynamically choose the cheapest representation satisfying performance constraints.

Conceptually:

```text
analyze(region)

if all entries identical:
    UNIFORM

else if occupancy/variation is high:
    DENSE

else if a few isolated entries exist:
    SPARSE

else if long repeated ranges dominate:
    RUN
```

The key challenge is that representation selection itself must not destroy allocator performance.

Possible policies:

```text
allocation-time conversion
background conversion
threshold-triggered conversion
never-convert-after-creation
periodic adaptation
```

These need benchmarking.

---

# 7. Representation Transitions

A region might begin:

```text
UNIFORM
```

Then become irregular:

```text
UNIFORM
   ↓
DENSE
```

Or:

```text
SPARSE
   ↓
DENSE
```

Potential state transitions:

```text
             ┌─────────┐
             │ UNIFORM │
             └────┬────┘
                  │
          exceptions appear
                  │
          ┌───────┴───────┐
          ↓               ↓
       SPARSE            RUN
          │               │
          └───────┬───────┘
                  ↓
                DENSE
```

Conversions must be evaluated carefully because converting representations costs CPU time and may require synchronization.

One possible philosophy:

> Optimize lookup aggressively while allowing modification operations to be somewhat more expensive.

This is attractive if lookups dominate mutations.

Whether that assumption holds depends on allocator workload.

---

# 8. Descriptor Design

One possibility is a compact tagged descriptor.

Conceptually:

```text
63                                                   0
+---------+------------+-------------+----------------+
|  type   | size class | generation  |    payload     |
+---------+------------+-------------+----------------+
```

Possible descriptor information:

```text
representation type
allocation size class
state
generation
pointer/index to secondary metadata
inline metadata
ownership information
```

The exact bit layout **must not be fixed prematurely**.

Different targets have:

```text
32-bit pointers
64-bit pointers
different canonical VA widths
different alignment guarantees
different pointer tagging rules
different ABI requirements
```

Therefore descriptor layout may need target-specific implementations while retaining identical logical semantics.

---

# 9. Inline Metadata

A particularly useful optimization may be allowing common metadata to live directly inside a descriptor.

Instead of:

```text
region descriptor
   ↓
metadata pointer
   ↓
metadata object
```

we may sometimes do:

```text
region descriptor
   ↓
metadata immediately available
```

For example:

```c
if (descriptor_is_uniform(descriptor))
{
    return descriptor_inline_class(descriptor);
}
```

This potentially removes another memory access.

Cache misses matter more than instruction count in many cases, so eliminating a dependent load can be more important than eliminating several arithmetic instructions.

---

# 10. Lifetime Model

During brainstorming, the idea arose of separating:

```text
saved addresses
temporary / unsaved addresses
```

in a manner loosely inspired by MIPS calling convention registers:

```text
$sN -> saved
$tN -> temporary
```

That analogy helped identify the problem, but it should **not become MemX semantics**.

MIPS `$s` and `$t` registers describe calling-convention responsibilities.

They do not represent heap object lifetime.

A cleaner lifetime model is:

```text
ACTIVE
RETIRED
RECLAIMABLE
```

---

# 11. Allocation States

## ACTIVE

The allocation is currently valid.

```text
ACTIVE
```

Normal lookups should resolve successfully.

---

## RETIRED

The allocation has logically been freed, but its backing storage cannot yet safely be reused.

```text
ACTIVE
   ↓
free()
   ↓
RETIRED
```

This state is useful for concurrent reclamation strategies.

---

## RECLAIMABLE

No valid reader should still rely on the old allocation.

```text
RETIRED
   ↓
safe reclamation condition
   ↓
RECLAIMABLE
```

Its memory can now be reused.

---

# 12. Lifetime State Machine

```text
             allocate
                │
                ▼
           ┌────────┐
           │ ACTIVE │
           └────┬───┘
                │
              free
                │
                ▼
          ┌─────────┐
          │ RETIRED │
          └────┬────┘
               │
         safe to reuse
               │
               ▼
      ┌────────────────┐
      │ RECLAIMABLE    │
      └───────┬────────┘
              │
            reuse
              │
              └──────────────► ACTIVE
```

---

# 13. Why Not Separate Saved/Unsaved Tables?

An early idea was:

```text
saved addresses -> table A
temporary addresses -> table B
```

This immediately creates another problem:

```text
What if something referenced through table A points to something in table B?
```

Now the implementation may require:

```text
lookup A
lookup B
cross-reference
cache
synchronization
```

Caching can reduce latency but increases memory usage.

That recreates the exact latency-vs-memory problem MemX is supposed to improve.

A cleaner approach is:

```text
one address index
        │
        ▼
    descriptor
        │
        ▼
      state
```

Everything remains within one namespace.

---

# 14. Reclamation

The original brainstorming included:

> clear unused addresses concurrently while serving addresses that are still being used.

That is an important concern, but **coroutines should not define the algorithm**.

Coroutines are a scheduling mechanism.

The underlying memory-safety problem is reclamation.

Potential reclamation strategies include:

```text
epoch-based reclamation
hazard pointers
quiescent-state reclamation
reference counting
deferred reclamation queues
allocator-specific techniques
```

MemX should not invent one unnecessarily if an established reclamation model satisfies the requirements.

---

# 15. Epoch-Style Reclamation Concept

One candidate model:

```text
global epoch = E
```

Readers announce which epoch they are currently operating in.

A free operation can do:

```text
mark allocation RETIRED
record retirement epoch
```

Example:

```text
Thread A        Thread B        Reclaimer

epoch 40        epoch 40

free(X)
X retired @ 40

                enters 41

enters 41

                               no thread can
                               reference epoch 40

                               X -> RECLAIMABLE
```

This is not novel by itself.

Epoch-based memory reclamation is established prior art.

The interesting question is how efficiently MemX metadata can encode and interact with such state.

---

# 16. Important Separation of Concerns

MemX currently contains several related but separate problems.

They should remain conceptually distinct.

## Problem A — Lookup

```text
pointer -> metadata
```

This is the primary research target.

---

## Problem B — Lifetime

```text
Is this metadata associated with an active, retired, or reusable allocation?
```

---

## Problem C — Reclamation

```text
When may retired memory safely become reusable?
```

---

## Problem D — Allocation

```text
Where should new memory come from?
```

MemX does **not necessarily need to become a complete malloc replacement**.

The initial library can focus narrowly on:

```text
address metadata indexing
```

and integrate with allocators.

This keeps benchmark results interpretable.

---

# 17. The Actual Algorithmic Target

A more precise research problem is:

> Given an address `P`, determine its metadata entry using the minimum practical number of dependent loads, branches, and metadata bytes, while supporting arbitrary allocator layouts.

Inputs:

```text
pointer P
MemX index I
```

Output:

```text
metadata M
```

or:

```text
NOT_FOUND
```

Possible lookup procedure:

```text
1. Convert P into an integer address.
2. Extract region index.
3. Load region descriptor.
4. Decode representation tag.
5. Resolve metadata using representation-specific logic.
6. Validate state/generation where required.
7. Return metadata.
```

This qualifies as an algorithm once all steps and representations are formally defined.

---

# 18. Preliminary Lookup Pseudocode

```c
memx_metadata_t *
memx_lookup(
    const memx_index_t *index,
    const void *pointer
)
{
    uintptr_t address;
    uintptr_t region_index;
    memx_descriptor_t descriptor;

    address = (uintptr_t)pointer;

    region_index =
        address >> MEMX_REGION_SHIFT;

    descriptor =
        index->regions[region_index];

    switch (memx_descriptor_type(descriptor))
    {
        case MEMX_REGION_UNIFORM:
        {
            return memx_uniform_lookup(
                descriptor,
                address
            );
        }

        case MEMX_REGION_DENSE:
        {
            return memx_dense_lookup(
                descriptor,
                address
            );
        }

        case MEMX_REGION_SPARSE:
        {
            return memx_sparse_lookup(
                descriptor,
                address
            );
        }

        case MEMX_REGION_RUN:
        {
            return memx_run_lookup(
                descriptor,
                address
            );
        }

        default:
        {
            return NULL;
        }
    }
}
```

This is **reference design pseudocode**, not an optimized implementation.

---

# 19. Branch Elimination

Eventually, the central dispatch:

```c
switch (type)
```

may itself be too expensive or unpredictable.

Potential approaches:

```text
tag-derived branchless resolution
function pointer dispatch
computed dispatch
representation-specific top-level tables
inline bit tricks
profile-guided specialization
```

But no optimization should be assumed superior.

Modern CPUs are complicated.

A supposedly clever branchless implementation can easily lose against a predictable branch.

Benchmark first.

---

# 20. Cache Behavior

MemX should primarily be designed around the memory hierarchy.

Ideal hot lookup:

```text
pointer
   ↓
descriptor load
   ↓
metadata
```

Target:

```text
few dependent memory accesses
few unpredictable branches
small hot metadata footprint
high L1/L2 residency
```

A structure using eight instructions and one cache miss may lose badly against a structure using twenty instructions with everything in L1.

Therefore benchmark metrics must include hardware events, not only wall-clock time.

---

# 21. Portable Core

MemX should have a portable reference implementation written in **C**.

Likely language baseline:

```text
C23
```

Potential fallback:

```text
C17
```

if toolchain/platform support becomes a meaningful constraint.

The C implementation defines:

```text
behavior
data structure semantics
invariants
reference results
portable fallback
```

Assembly should optimize those semantics.

Assembly should **not define separate semantics**.

---

# 22. C ABI

A stable C ABI makes MemX usable from many languages.

Potential initial API shape:

```c
#include <memx.h>
```

Example:

```c
memx_metadata_t *
memx_lookup(
    const memx_index_t *index,
    const void *pointer
);
```

Possible future API:

```c
memx_result_t
memx_insert(
    memx_index_t *index,
    void *base,
    size_t size,
    const memx_metadata_t *metadata
);

memx_result_t
memx_remove(
    memx_index_t *index,
    const void *base
);

memx_result_t
memx_retire(
    memx_index_t *index,
    const void *base
);
```

Names are provisional.

---

# 23. Pointer Width

Never assume:

```text
64-bit pointer == universal reality
```

Use:

```c
uintptr_t
```

for pointer-sized arithmetic.

Example:

```c
uintptr_t address;

address = (uintptr_t)pointer;
```

This naturally supports architectures with different pointer widths where `uintptr_t` exists.

Potential checks:

```c
#if UINTPTR_MAX == UINT64_MAX
    /* 64-bit */
#elif UINTPTR_MAX == UINT32_MAX
    /* 32-bit */
#else
    /* unusual target */
#endif
```

But MemX should avoid unnecessary compile-time assumptions whenever generic `uintptr_t` arithmetic works.

---

# 24. Architecture Philosophy

Target as many practical architectures as possible.

Initial ambition:

```text
x86
x86-64

ARM32
AArch64

RISC-V 32
RISC-V 64

MIPS32
MIPS64

PowerPC 32
PowerPC 64
```

Potential later targets:

```text
s390x
LoongArch
SPARC
WebAssembly
CHERI-capability architectures
other viable targets
```

Support should be based on actual build/test capability rather than claiming an architecture because a few macros compile.

---

# 25. Architecture-Specific Optimizations

The algorithm remains logically identical.

Hot primitives can receive ISA-specific implementations.

For example:

```text
region = address >> 21
```

### x86-64

```asm
shr rax, 21
```

### AArch64

```asm
lsr x0, x0, #21
```

### MIPS64

```asm
dsrl $t0, $a0, 21
```

Same operation.

Different lowering.

---

# 26. x86 / x86-64 Opportunities

Potentially useful extensions:

```text
BMI
BMI2
LZCNT
TZCNT
POPCNT
AVX2
AVX-512
```

Possible uses:

```text
bit extraction
bitmap rank
sparse metadata lookup
mask generation
descriptor decoding
parallel comparison
```

Important:

Instructions such as `PEXT` may look perfect theoretically but have very different throughput/latency across microarchitectures.

Never assume an ISA extension wins merely because the instruction count is smaller.

---

# 27. ARM / AArch64 Opportunities

Potential tools:

```text
UBFX
BFI/BFXIL
shifted operands
conditional select
load pairs
NEON
SVE
```

AArch64's bitfield instructions could be especially useful for packed descriptors.

---

# 28. RISC-V Opportunities

Base implementation:

```text
RV32I
RV64I
```

Potential extensions:

```text
Zbb
Zbs
Zba
Zbc
Vector extension
```

Keep extension-specific paths optional.

For example:

```text
generic RV64
RV64 + Zbb
RV64 + Zbs
```

Runtime or compile-time dispatch can choose the best supported path.

---

# 29. MIPS Opportunities

Possible targets:

```text
MIPS32
MIPS64
```

Operations likely remain straightforward:

```text
shifts
masks
loads
comparisons
```

Older MIPS implementations may require consideration of:

```text
branch delay slots
load delays
ISA revisions
endianness
```

MIPS support should not assume all MIPS CPUs behave like one unified modern architecture.

---

# 30. PowerPC Opportunities

PowerPC's rotate-and-mask instructions may be particularly suitable for packed metadata extraction.

Potential primitives:

```text
rotate
mask
count leading zeros
bitfield manipulation
```

Again, architecture-specific tricks should only be retained when benchmarks show meaningful gains.

---

# 31. Endianness

MemX should support both:

```text
little endian
big endian
```

where practical.

Descriptor serialization and bitfield layout must not accidentally depend on host byte order.

Prefer explicit integer masks/shifts rather than C implementation-defined bitfields for persistent or cross-platform layouts.

Avoid:

```c
struct
{
    unsigned type : 3;
    unsigned size : 5;
};
```

for layouts whose exact representation matters.

Prefer:

```c
#define MEMX_TYPE_MASK ...
#define MEMX_TYPE_SHIFT ...
```

and explicit operations.

---

# 32. C vs Assembly Responsibilities

C:

```text
reference implementation
structure creation
representation conversion
slow paths
validation
generic lookup
testing oracle
```

Assembly:

```text
hot lookup primitives
descriptor decode
bitmap operations
specialized scan/rank
ISA-specific fast paths
```

Do **not** immediately rewrite the whole project in assembly.

That would create:

```text
10 architectures
×
several representations
×
multiple ABI variants
=
maintenance hell
```

Assembly should attack measured hotspots.

---

# 33. Repository Layout

Possible layout:

```text
memx/
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── include/
│   └── memx/
│       ├── memx.h
│       ├── types.h
│       └── config.h
│
├── src/
│   ├── core/
│   │   ├── index.c
│   │   ├── lookup.c
│   │   ├── insert.c
│   │   ├── remove.c
│   │   ├── reclaim.c
│   │   └── descriptor.c
│   │
│   ├── region/
│   │   ├── uniform.c
│   │   ├── dense.c
│   │   ├── sparse.c
│   │   └── run.c
│   │
│   └── arch/
│       ├── generic/
│       ├── x86/
│       ├── x86_64/
│       ├── arm/
│       ├── aarch64/
│       ├── riscv32/
│       ├── riscv64/
│       ├── mips32/
│       ├── mips64/
│       ├── ppc32/
│       └── ppc64/
│
├── tests/
│   ├── unit/
│   ├── randomized/
│   ├── concurrency/
│   └── differential/
│
├── bench/
│   ├── lookup/
│   ├── allocator/
│   ├── fragmentation/
│   └── traces/
│
├── bindings/
│   ├── cpp/
│   ├── rust/
│   ├── zig/
│   └── kotlin-native/
│
└── docs/
    ├── algorithm.md
    ├── architecture.md
    ├── benchmark.md
    └── prior-art.md
```

---

# 34. Language Targets

The core implementation should remain:

```text
C
+
assembly where justified
```

A stable C ABI allows bindings.

Suggested order:

```text
1. C
2. C++
3. Rust
4. Zig
5. Kotlin/Native
```

Potential later interfaces:

```text
Python
C#
Java
Go
Swift
D
Nim
```

These do not require separate implementations of the MemX algorithm.

They should call the core library.

---

# 35. C++ Binding

Potentially header-only:

```cpp
#include <memx/memx.hpp>

memx::index index;

auto metadata =
    index.lookup(pointer);
```

Keep abstraction zero-cost where possible.

---

# 36. Rust Binding

Likely split:

```text
memx-sys
memx
```

`memx-sys`:

```text
raw C ABI
```

`memx`:

```text
safe wrapper where sound semantics can be provided
```

Memory indexing is inherently unsafe territory, so the safe wrapper should not pretend arbitrary raw pointer use can magically become safe.

---

# 37. Zig Binding

Zig can directly consume the C ABI.

Potentially very little wrapper code required.

---

# 38. Kotlin/Native

Possible through:

```text
cinterop
```

This is lower priority but could provide interesting runtime experimentation.

---

# 39. Benchmark Targets

MemX should be compared against multiple baseline data structures.

At minimum:

```text
flat table
two-level table
radix tree
adaptive MemX
```

Where practical, create comparable implementations inspired by:

```text
TCMalloc PageMap concepts
jemalloc rtree concepts
snmalloc PageMap concepts
```

Avoid dishonest benchmark claims such as:

> MemX beats jemalloc

unless MemX is genuinely integrated into comparable allocator workloads.

A standalone lookup microbenchmark only proves:

> MemX lookup performed better under this lookup benchmark.

Nothing more.

---

# 40. Workloads

Benchmark:

```text
random lookup
sequential lookup
random frees
sequential frees
mixed allocation sizes
small allocations
large allocations
high fragmentation
low fragmentation
uniform regions
sparse regions
run-heavy regions
worst-case irregular regions
hot-cache operation
cold-cache operation
cross-thread operation
```

---

# 41. Hardware Metrics

Do not benchmark only:

```text
ns/op
```

Collect:

```text
cycles / lookup
instructions / lookup
IPC
branches
branch misses
L1D loads
L1D misses
L2 misses
LLC misses
TLB misses
metadata bytes
metadata bytes / managed MiB
peak RSS
```

On Linux, `perf` will be central.

Example:

```bash
perf stat \
    -e cycles \
    -e instructions \
    -e branches \
    -e branch-misses \
    -e cache-references \
    -e cache-misses \
    ./memx_bench
```

More detailed microarchitecture-specific counters can be added later.

---

# 42. The Main Performance Graph

A central MemX result should be the tradeoff between:

```text
lookup latency
```

and:

```text
metadata footprint
```

Conceptually:

```text
lookup
latency
  ^
  |
  |      radix
  |        *
  |
  |                 adaptive index
  |                       *
  |
  |                           MemX?
  |                              *
  |
  |                                  flat table
  |                                      *
  +-------------------------------------------->
                 metadata memory
```

The goal is not necessarily:

> absolute fastest lookup under all circumstances.

A more useful win could be:

> near-flat-table latency at substantially lower metadata cost.

That is a real systems tradeoff.

---

# 43. Benchmark Correctness

Microbenchmarks are easy to accidentally fake.

Need to prevent:

```text
compiler removing operations
constant folding
dead-code elimination
unrealistic cache warming
predictable synthetic addresses
single fixed data layout
```

Results should include:

```text
median
p95
p99
minimum
maximum
variance
```

and preferably repeated independent runs.

CPU frequency scaling and thermal behavior should be controlled where possible.

---

# 44. Differential Testing

Every architecture-specific fast path should be compared against the portable C implementation.

Conceptually:

```c
expected =
    memx_generic_lookup(index, pointer);

actual =
    memx_arch_lookup(index, pointer);

assert(expected == actual);
```

Generate millions of randomized layouts.

This allows assembly optimization without silently creating architecture-specific correctness bugs.

---

# 45. Fuzzing

Fuzz:

```text
region creation
representation transitions
pointer boundaries
invalid pointers
maximum addresses
zero addresses
alignment edge cases
region boundaries
descriptor corruption detection
concurrent state changes
```

Useful tools may include:

```text
libFuzzer
AFL++
Honggfuzz
custom randomized harnesses
```

---

# 46. Sanitizers

Portable builds should regularly run:

```text
ASan
UBSan
TSan
MSan where practical
```

Assembly fast paths complicate sanitizer visibility, making the C reference version even more important.

---

# 47. Compiler Matrix

Potential baseline:

```text
GCC
Clang
MSVC where supported
```

Strict flags during development.

For GCC/Clang:

```text
-Wall
-Wextra
-Wpedantic
-Werror
-Wconversion
-Wshadow
```

Potential language mode:

```text
-std=c23
```

---

# 48. Platform Targets

Initial primary development platform:

```text
Linux
```

because it provides:

```text
perf
allocator experimentation
mmap
NUMA tooling
hugepage controls
hardware counters
easy cross-compilation
```

Primary architectures:

```text
x86-64
AArch64
```

Then expand.

Windows can follow once the fundamental design stabilizes.

---

# 49. Region Size

A major design parameter is:

```text
MEMX_REGION_SHIFT
```

For example:

```text
2 MiB regions -> shift 21
```

but that number **must not be chosen because 21 looks convenient**.

We need to benchmark region sizes.

Candidates:

```text
64 KiB
256 KiB
1 MiB
2 MiB
4 MiB
16 MiB
```

Tradeoffs:

Small regions:

```text
+ more likely uniform
+ smaller local tables
- larger top-level directory
```

Large regions:

```text
+ smaller top-level directory
- more internal irregularity
- secondary representations become larger
```

There may be a workload-dependent optimum.

---

# 50. Granularity

Likewise, metadata granularity might be:

```text
allocation
page
allocator span
fixed block
custom granule
```

MemX should likely begin at **page/span-style granularity** rather than arbitrary byte-level indexing.

Byte-level direct metadata indexing would be absurdly expensive.

---

# 51. Huge Allocations

Huge allocations may bypass normal small-object indexing.

Potential representation:

```text
HUGE
```

Example:

```text
descriptor:
    type = HUGE
    base
    length
    metadata
```

Whether `HUGE` deserves its own representation depends on actual data.

---

# 52. Concurrency

Lookup should ideally be:

```text
read-mostly
lock-free or effectively lock-free
```

Modification operations may tolerate more synchronization.

Possible design goal:

```text
lookup:
    zero locks

insert:
    localized synchronization

remove:
    retirement rather than immediate destruction

representation conversion:
    publish new immutable representation
```

This leads naturally toward copy/publish patterns.

---

# 53. Immutable Region Snapshots

One possible concurrency strategy:

```text
Region representation is immutable once published.
```

Modification builds a replacement:

```text
old region
   ↓
construct new region
   ↓
atomic publish
   ↓
retire old representation
   ↓
reclaim later
```

Advantages:

```text
simple readers
no partial state
easy lock-free lookup
```

Costs:

```text
conversion allocations
temporary duplicate metadata
reclamation complexity
```

This should be explored rather than assumed.

---

# 54. Atomic Publication

Potential pattern:

```c
atomic_store_explicit(
    &index->regions[i],
    new_descriptor,
    memory_order_release
);
```

Readers:

```c
descriptor = atomic_load_explicit(
    &index->regions[i],
    memory_order_acquire
);
```

But memory ordering should be proven from invariants, not copied mechanically.

A weaker ordering may suffice.

A stronger ordering may be required.

The design needs a real concurrency proof.

---

# 55. NUMA

Eventually MemX should consider NUMA.

A single global metadata structure can introduce remote-memory traffic.

Possible strategies:

```text
per-NUMA-node directories
local replicas
ownership partitioning
region affinity
```

Not v0.

But architecture should avoid making future NUMA support impossible.

---

# 56. False Sharing

Descriptors frequently modified by different threads should not accidentally share cache lines.

Likewise, reader-heavy metadata should be separated from mutation-heavy metadata if benchmarks show coherence traffic.

Potential split:

```text
read descriptor
mutable lifecycle state
```

rather than packing everything into one cache line.

Again: measure first.

---

# 57. Generation Values

Generation identifiers can help distinguish reuse.

Example:

```text
address X
generation 14
```

is freed.

Later the same address:

```text
address X
generation 15
```

belongs to a new allocation.

Metadata can then distinguish:

```text
same virtual address
different allocation lifetime
```

Generation width creates wraparound concerns.

Possible widths:

```text
8 bits
16 bits
32 bits
```

Trade memory against wrap frequency.

---

# 58. Stale Pointer Detection

A possible secondary feature:

```text
pointer + expected generation
```

could detect some stale references.

However, MemX should not turn into a full memory sanitizer unless deliberately expanded.

Primary objective remains metadata lookup performance.

---

# 59. Address Canonicalization

Different architectures have different virtual address rules.

For example x86-64 does not necessarily use all 64 address bits.

AArch64 may use:

```text
TBI
PAC
MTE
```

depending on configuration.

MemX must not blindly assume:

```c
(uintptr_t)p
```

contains only plain address bits on every target.

Architecture layers may need to normalize pointers before indexing.

---

# 60. Tagged Pointers

Potential environments:

```text
AArch64 TBI
language runtimes
custom allocator tags
capability systems
```

MemX should define whether tags are:

```text
ignored
validated
preserved
part of lookup
```

This needs explicit API semantics.

---

# 61. Security

Metadata indexes operate on raw addresses, which makes hardening relevant.

Potential threats:

```text
invalid pointers
maliciously crafted pointers
integer overflow
out-of-range region indexes
corrupted descriptors
use-after-free
metadata tampering
```

Bounds and arithmetic must be carefully checked on public APIs.

Fast allocator-internal paths may offer separate unchecked APIs if justified.

---

# 62. Checked vs Unchecked API

Potentially:

```c
memx_lookup()
```

safe/checked public lookup.

And:

```c
memx_lookup_unchecked()
```

for allocator internals where address range validity is already guaranteed.

This allows security checks without forcing them into every internal hot path.

---

# 63. API Naming

Project:

```text
MemX
```

Library:

```text
libmemx
```

Header:

```c
#include <memx.h>
```

or:

```c
#include <memx/memx.h>
```

Suggested prefix:

```text
memx_
```

Examples:

```c
memx_init
memx_destroy
memx_lookup
memx_insert
memx_remove
memx_retire
memx_reclaim
```

---

# 64. Potential Package Names

```text
memx
libmemx
memx-rs
memx-sys
memx-cpp
```

Check ecosystem/name collisions before public release.

---

# 65. Non-Goals for Early Versions

MemX v0 should **not** attempt all of these simultaneously:

```text
replace malloc
replace jemalloc
be a garbage collector
perform automatic object ownership
be a leak detector
be a memory sanitizer
be a VM manager
compress arbitrary memory
provide every language runtime
```

That would destroy focus.

Early objective:

> Build and measure a new adaptive pointer-to-metadata indexing structure.

---

# 66. Research Method

Development should happen in stages.

## Stage 1 — Baselines

Implement:

```text
flat table
two-level table
simple radix tree
```

Measure them.

---

## Stage 2 — MemX Uniform/Dense

Implement only:

```text
UNIFORM
DENSE
```

This is enough to test whether adaptive representations have merit.

---

## Stage 3 — Sparse

Add:

```text
SPARSE
```

Measure when it wins.

---

## Stage 4 — Runs

Add:

```text
RUN
```

Measure when it wins.

---

## Stage 5 — Adaptive Selection

Create a policy for choosing representations.

---

## Stage 6 — Concurrency

Add atomic publication and reclamation.

---

## Stage 7 — ISA Optimization

Only after identifying real hotspots.

---

## Stage 8 — Allocator Integration

Integrate with a small allocator or experimental allocator frontend.

---

## Stage 9 — Compare Against Production Designs

Only then make stronger claims.

---

# 67. First Prototype

The first prototype should be intentionally boring.

Something like:

```c
typedef enum
{
    MEMX_REGION_EMPTY,
    MEMX_REGION_UNIFORM,
    MEMX_REGION_DENSE
} memx_region_type_t;
```

Then:

```c
typedef struct
{
    memx_region_type_t type;

    union
    {
        memx_metadata_t *uniform;
        memx_metadata_t **dense;
    };
} memx_region_t;
```

This will not be optimal.

That is fine.

Its purpose is to validate behavior.

---

# 68. First Benchmark Question

Do not initially ask:

> Does MemX beat jemalloc?

Ask:

> For synthetic region layouts with increasing irregularity, when does a uniform/dense adaptive index beat a flat map or radix lookup in memory footprint and lookup latency?

That question is answerable.

---

# 69. Benchmark Dataset Families

Generate layouts such as:

## Completely uniform

```text
AAAAAAAAAAAAAAAAAAAAAAAA
```

## Mostly uniform

```text
AAAAAAAAAABAAAAAAAAAAAAA
```

## Runs

```text
AAAAAAAABBBBBBBBCCCCCCCC
```

## Sparse

```text
A.......B..........C....
```

## Random

```text
ABCEADFFCBAGEDAFBCDE....
```

## Fragmented allocator-like

Generated using realistic allocation/free traces.

---

# 70. Real Traces

Synthetic benchmarks are not sufficient.

Eventually collect traces from:

```text
compilers
web servers
game workloads
build systems
language runtimes
database workloads
browser-like workloads
```

Record:

```text
allocation sizes
allocation addresses
free order
lifetimes
thread ownership
```

Replay into MemX.

---

# 71. Architecture Benchmark Matrix

Example eventual table:

| ISA     | Generic C | Tuned C |   ASM |
| ------- | --------: | ------: | ----: |
| x86-64  |       yes |     yes |   yes |
| AArch64 |       yes |     yes |   yes |
| RV64    |       yes |     yes |   yes |
| MIPS64  |       yes |     yes |   yes |
| PPC64   |       yes |     yes |   yes |
| x86     |       yes |   later | later |
| ARM32   |       yes |   later | later |
| RV32    |       yes |   later | later |

No architecture gets marked supported until correctness tests actually run on it.

---

# 72. Cross Compilation

Potential tooling:

```text
Clang
GCC cross toolchains
QEMU
native runners
CI hardware
```

QEMU is useful for correctness.

It is generally **not valid for native microarchitecture performance claims**.

Performance numbers require real hardware.

---

# 73. Assembly Dispatch

Potential model:

```text
memx_lookup
   ↓
CPU capability detection
   ↓
best available implementation
```

For example x86:

```text
generic x86-64
BMI1
BMI2
AVX2
```

But dispatch overhead should not occur on every call.

Resolve once during initialization or through IFUNC-like mechanisms where appropriate.

---

# 74. Compiler Intrinsics Before Assembly

Before handwritten assembly:

```text
try portable C
inspect compiler output
try intrinsics
benchmark
```

Only use assembly if it produces a measurable improvement the compiler cannot reliably achieve.

This prevents writing thousands of lines of decorative assembly that compilers already generate perfectly.

---

# 75. Formal Invariants

Eventually MemX needs invariants such as:

```text
Every indexed address belongs to at most one active metadata owner.

Every published descriptor is immutable to lock-free readers.

A RETIRED descriptor remains reachable until all valid readers can no longer reference it.

Region representation transitions preserve lookup equivalence.

Every representation resolves the same metadata for every valid address.
```

These are more important than fancy bit tricks.

---

# 76. Complexity Targets

Ideal lookup:

```text
O(1)
```

in terms of fixed hierarchy depth.

But asymptotic complexity alone is nearly useless here.

We care about:

```text
dependent loads
branch predictability
cache residency
metadata size
TLB pressure
```

Two `O(1)` algorithms can differ by an order of magnitude.

---

# 77. Mutation Complexity

Insert/remove may be allowed to cost more.

Possible philosophy:

```text
Lookup:
    aggressively optimized

Insert:
    moderate cost acceptable

Remove:
    retirement cheap

Reclamation:
    amortized/background
```

This should be driven by real allocator ratios.

---

# 78. Memory Overhead Formula

MemX should eventually provide explicit overhead formulas.

For example:

```text
TopLevelBytes
+
UniformDescriptorBytes
+
DenseTableBytes
+
SparseStructureBytes
+
RunStructureBytes
+
ReclamationBytes
```

Then report:

```text
bytes of metadata / MiB managed
```

This is a much more useful measure than total process memory alone.

---

# 79. Compression Connection

The project originally considered building something LZ4-like.

That idea influenced MemX indirectly.

The adaptive region design is essentially trying to exploit **regularity in allocator metadata**.

Examples:

```text
uniform repetition
runs
sparsity
local density
```

So MemX contains a compression-like idea:

> Compress metadata representation without making lookup expensive.

This differs from general-purpose compression.

The objective is:

```text
random-access metadata compression
```

rather than:

```text
maximum compression ratio
```

That distinction may become important.

---

# 80. Possible Deeper Direction: Random-Access Metadata Compression

One potentially stronger description of MemX is:

> **A random-access compression scheme specialized for virtual-address metadata.**

Normal compression optimizes:

```text
size
```

while accepting decompression work.

MemX needs:

```text
compressed structure
+
extremely cheap random lookup
```

That may be the actual intellectual center of the project.

---

# 81. Representation Selection as an Optimization Problem

Given a region `R`, select representation `T` minimizing:

```text
Cost(T, R)
```

Potential cost:

```text
Cost =
    α * expected_lookup_cycles
  + β * metadata_bytes
  + γ * mutation_cost
```

where:

```text
α
β
γ
```

represent policy weights.

Different applications might choose:

```text
speed-first
balanced
memory-first
```

This could eventually make the adaptive selection policy formally defined rather than heuristic soup.

---

# 82. Static vs Dynamic Adaptation

Two possible modes:

## Static

Choose representation when region is created.

Do not change until destroyed.

Advantages:

```text
simple
stable
cheap
```

---

## Dynamic

Change as layout evolves.

Advantages:

```text
better adaptation
```

Costs:

```text
conversion
synchronization
reclamation
complexity
```

Static adaptation should probably come first.

---

# 83. Potential Naming for the Algorithm

Project:

```text
MemX
```

Possible internal algorithm names:

```text
ARI — Adaptive Region Index

MRI — MemX Region Index

AMRI — Adaptive Memory Region Index

XRI — eXtensible Region Index
```

No need to force a research-paper acronym before the actual algorithm exists.

---

# 84. What Would Count as Success?

Strong result:

```text
MemX:
    lookup within ~flat-table territory
    significantly smaller metadata footprint
```

Another good result:

```text
MemX:
    same memory as radix structure
    fewer dependent loads
    lower lookup latency
```

Even a conditional win matters:

```text
MemX wins heavily on low-fragmentation allocator workloads
but loses on high-entropy layouts.
```

That still reveals something useful.

---

# 85. What Would Count as Failure?

Possible result:

```text
adaptive dispatch costs more than it saves
```

or:

```text
real heaps are too irregular
```

or:

```text
representation transitions are too expensive
```

or:

```text
existing radix/page-map structures already dominate the Pareto frontier
```

That is still valuable research.

The project then explains **why**.

---

# 86. No Fake Novelty Claims

Do not market MemX as:

> the world's first fast heap lookup algorithm.

That would be obviously wrong.

Production allocators have spent decades optimizing this.

Until prior-art research is deeper, use language like:

```text
experimental
adaptive
research
prototype
investigating
```

If the adaptive representation turns out to already exist essentially unchanged somewhere, we adjust.

That is normal research.

---

# 87. Immediate Prior-Art Targets

Study these deeply:

```text
TCMalloc PageMap
jemalloc rtree
snmalloc address-space metadata
mimalloc segment/page metadata
Linux kernel radix/xarray concepts
page-table structures
compressed radix trees
adaptive radix trees
bitmap indexes
succinct data structures
interval maps
epoch reclamation
RCU
hazard pointers
```

We are likely to steal concepts, not code.

Then identify what combination is genuinely different.

---

# 88. Initial MemX v0 Scope

MemX v0:

```text
C23
Linux
x86-64

portable lookup interface

UNIFORM
DENSE

single-threaded mutations

benchmark suite

flat-table baseline
two-level baseline
radix baseline
```

No assembly yet.

No reclamation yet.

No Rust bindings yet.

No Windows yet.

No ten-architecture support yet.

The architecture should permit all of those later.

---

# 89. MemX v1 Direction

After v0 proves anything useful:

```text
SPARSE
RUN

thread-safe reads

descriptor packing

AArch64

x86-64 assembly/intrinsics

Rust/C++ bindings
```

---

# 90. MemX v2 Direction

Possible:

```text
RISC-V
MIPS
PowerPC

dynamic adaptation

epoch reclamation

allocator integration

NUMA experiments

runtime CPU dispatch
```

---

# 91. Example Public Description

> **MemX is an experimental C library for high-performance pointer-to-metadata indexing. It investigates adaptive region representations that trade metadata density against lookup latency, with portable C semantics and architecture-specific optimized fast paths.**

Short version:

> **Adaptive pointer metadata indexing for low-level allocators and runtimes.**

---

# 92. Example Usage Vision

Eventually:

```c
#include <memx/memx.h>

int
main(void)
{
    memx_index_t index;

    if (memx_init(&index) != MEMX_OK)
    {
        return 1;
    }

    void *pointer = /* allocator-owned address */;

    memx_metadata_t *metadata =
        memx_lookup(&index, pointer);

    if (metadata != NULL)
    {
        /* use metadata */
    }

    memx_destroy(&index);

    return 0;
}
```

Actual API should emerge from implementation needs.

---

# 93. Core Principle

MemX should always preserve this hierarchy:

```text
Correctness
    ↓
Algorithm
    ↓
Data layout
    ↓
Cache behavior
    ↓
Compiler output
    ↓
ISA-specific optimization
```

Not:

```text
write assembly
    ↓
hope algorithm appears
```

---

# 94. Main Hypothesis

The hypothesis worth testing is:

> Real allocator address metadata contains enough local regularity that using multiple region-specific representations can reduce metadata footprint without paying the full lookup cost of a generic hierarchical structure.

That is the idea.

Everything else supports testing it.

---

# 95. First Concrete Experiment

Implement:

```text
region size = configurable

representation:
    EMPTY
    UNIFORM
    DENSE
```

Generate heaps with varying entropy.

For each workload:

```text
1. populate metadata
2. generate 10M+ pointer lookups
3. randomize lookup order
4. measure lookup latency
5. measure cache misses
6. calculate metadata footprint
7. compare against flat table
8. compare against two-level table
```

Plot:

```text
cycles / lookup
vs
bytes metadata / managed MiB
```

If `UNIFORM + DENSE` cannot produce a useful tradeoff, stop and rethink before adding ten more representations.

---

# 96. The Big Picture

The original brainstorm went:

```text
linked lists
   ↓
what is slow for programmers?
   ↓
heap lookup?
   ↓
pointer -> metadata indexing
   ↓
how do we free while remaining fast?
   ↓
concurrent clearing?
   ↓
saved vs temporary addresses?
   ↓
cross-lookup problem
   ↓
cache?
   ↓
too much memory
   ↓
single metadata namespace
   ↓
lifetime states
   ↓
adaptive region representations
   ↓
portable algorithm
   ↓
per-ISA optimization
```

That is now MemX.

Not:

```text
"faster heap somehow"
```

but:

```text
                  MEMX

                    pointer
                       │
                       ▼
               region selection
                       │
                       ▼
               compact descriptor
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
      UNIFORM         DENSE       SPARSE/RUN
         │             │             │
         └─────────────┼─────────────┘
                       ▼
                    metadata
                       │
                       ▼
            ACTIVE / RETIRED /
               RECLAIMABLE
```

With the implementation strategy:

```text
portable C semantics
        │
        ▼
reference implementation
        │
        ▼
profiling
        │
        ▼
ISA-specific optimized paths
```

And the research goal:

```text
less metadata
+
fewer cache misses
+
fast random lookup
+
portable semantics
+
architecture-specific acceleration
```

---

# 97. Rule for the Project

Before adding any optimization, ask:

```text
What measured problem does this solve?
```

Before adding any representation:

```text
What real layout does this represent better?
```

Before writing assembly:

```text
What instruction/cache bottleneck did the profiler show?
```

Before claiming victory:

```text
What baseline did we beat, under what workload, on what hardware?
```

If MemX follows those rules, even a failed experiment will produce useful systems knowledge.

If it doesn't, it'll turn into 20,000 lines of extremely fast-looking vibes.

We are avoiding the second one. 💀

---

# 98. Current Status

```text
Project name:      MemX
Core language:     C
Optimized paths:   Assembly / intrinsics per ISA
Primary problem:   pointer -> metadata lookup
Main technique:    adaptive region representation
Lifetime model:    ACTIVE -> RETIRED -> RECLAIMABLE
Current stage:     research / algorithm design
Novelty status:    unproven
First platform:    Linux x86-64
Long-term target:  broad ISA support
```

---

# 99. Next Step

Do not write the allocator yet.

Do not write the MIPS implementation yet.

Do not write reclamation yet.

The immediate next task is:

> **Define the smallest possible MemX region format and benchmark ****`UNIFORM + DENSE`**** against flat and two-level lookup.**

If that demonstrates something interesting, continue.

If it does not, change the algorithm before the codebase becomes expensive to change.

---

# 100. One-Line Mission

> **MemX investigates whether allocator metadata can be compressed according to local address-space structure without sacrificing near-direct-map lookup performance.**
