# MemX v0 Design Decisions

## Status vocabulary

Each decision is one of:

- accepted;
- provisional;
- deferred;
- rejected.

Accepted means Gate 1 code depends on it.

Provisional means experiments may replace it.

Deferred means it is intentionally outside v0.

Rejected means it was considered and excluded from the current direction.

## D1: Index allocator-defined granules

Status: accepted.

MemX v0 maps an address to allocator-defined granule metadata.

It does not promise individual-object resolution.

Several objects may occupy one indexed granule.

The returned handle may identify span metadata.

The allocator can derive the individual object afterward.

Reasoning:

- page and span indexing matches existing allocator needs;
- byte-level metadata would be unreasonably large;
- object boundaries can be allocator-specific;
- one generic object model would narrow integrations.

Consequence:

The public documentation says “granule/span metadata.”

Benchmarks use equal granule semantics for every structure.

## D2: Use one native-word result

Status: accepted.

The hot lookup returns `memx_handle_t`.

`memx_handle_t` is `uintptr_t`.

`MEMX_HANDLE_INVALID` is `UINTPTR_MAX`.

Zero is a valid handle.

Every other native-word value is valid.

Reasoning:

- one result normally uses one ABI return register;
- `{found, handle}` may require multiple registers or ABI-specific packing;
- callers rarely need every possible opaque value;
- a reserved sentinel keeps checked lookup simple.

Consequence:

Callers must not insert the sentinel.

Large handles that cannot be inline-encoded use DENSE storage.

## D3: Separate checked and assumed-mapped paths

Status: accepted.

`memx_lookup_address` checks index and range.

`memx_bounded_lookup_assume_mapped` assumes a valid bounded address.

`memx_flat_lookup_assume_mapped` assumes a valid optimized address.

Reasoning:

- public callers need safe bounds behavior;
- allocator internals often prove range validity earlier;
- benchmark labels must expose the semantic difference;
- hiding unchecked assumptions creates dishonest comparisons.

Consequence:

Assumed-mapped functions are inline view accessors.

Passing an out-of-range address is caller error.

## D4: Start with EMPTY, UNIFORM, and DENSE

Status: accepted.

Gate 1 contains exactly three adaptive representation states.

EMPTY stores no secondary metadata.

UNIFORM stores a common handle inline when possible.

DENSE stores one handle per granule.

Reasoning:

- this is the smallest experiment that tests local regularity;
- SPARSE and RUN would add policy variables before the hypothesis is tested;
- representation count itself can increase dispatch entropy;
- early simplicity enables exhaustive enumeration.

Consequence:

No RUN implementation exists.

No within-region SPARSE implementation exists.

The word “sparse” in v0 directory mode refers to directory coverage only.

## D5: Use a tagged native-word descriptor

Status: accepted for the portable prototype.

Descriptor zero means EMPTY.

Low tag `01` means inline UNIFORM.

Low tag `10` means an aligned DENSE pointer.

Two low bits are reserved for tags.

Reasoning:

- common lookup begins with one descriptor load;
- common uniform metadata needs no dependent load;
- `malloc` alignment supplies the required low zero bits;
- masks and shifts avoid implementation-defined C bitfields.

Consequence:

An allocator returning insufficiently aligned memory causes allocation failure.

The descriptor encoding is internal, not a stable serialized ABI.

## D6: Preserve the handle domain

Status: accepted.

Inline encoding can store handles through `UINTPTR_MAX >> 2`.

Larger valid handles are represented by a filled DENSE table.

Reasoning:

- the API must not have undocumented value-dependent rejection;
- opaque handles may contain high bits;
- correctness matters more than forcing every uniform layout inline.

Consequence:

A logically uniform region can report `MEMX_REGION_DENSE`.

The diagnostic type describes physical representation.

## D7: Provide bounded and sparse directories

Status: accepted.

BOUNDED covers one explicit aligned range.

It stores one descriptor word per covered region.

SPARSE covers a configured low-bit address space.

It stores a direct root and lazy leaves.

Reasoning:

- bounded lookup tests the one-load fast path;
- sparse coverage prevents directory memory from being ignored;
- separate labels keep memory accounting honest;
- a two-level sparse directory is simple enough for Gate 1.

Consequence:

BOUNDED and SPARSE results are reported separately.

The SPARSE directory rejects more than 32 region-index bits in v0.

## D8: Allow global flat fallback

Status: provisional.

A fully dense bounded index may convert to a direct flat table.

Conversion is explicit through `memx_index_optimize`.

Partial density is not converted globally.

Reasoning:

- adaptive dispatch should not doom the worst case;
- a random fully dense workload has no compressible local regularity;
- the direct table is the honest best representation for that shape;
- explicit conversion keeps creation and lookup behavior observable.

Consequence:

The dense Gate 1 candidate is named `memx_flat_fallback`.

It is not presented as adaptive compression.

It is a safe degradation mode.

## D9: Keep mutations single-threaded in Gate 1

Status: accepted.

No v0 synchronization guarantee is provided.

Readers must not race mutations.

Reasoning:

- Gate 1 asks whether representation adaptation has value;
- concurrency would add publication and reclamation overhead;
- that overhead could obscure the initial hypothesis;
- a concurrency proof deserves its own gate.

Consequence:

Atomic descriptors are not used yet.

Reader guards are not public yet.

## D10: Abstract the future lifetime contract

Status: deferred.

Future guarded lookup should be wait-free.

Epoch/QSBR is a candidate implementation.

It is not an algorithmic dependency.

An embedding allocator may provide equivalent lifetime protection.

Reasoning:

- some allocators already have epochs or quiescence;
- duplicate protection would waste hot-path work;
- reclamation strategy and lookup representation are separate concerns.

Consequence:

Trace files can record RETIRE and QUIESCENT today.

The core v0 library does not interpret them.

## D11: Define trusted and validated hint modes

Status: provisional API reservation.

The configuration enum includes VALIDATED and TRUSTED.

Gate 1 has no optimization that consumes layout hints.

Reasoning:

- future arithmetic shortcuts may depend on allocator promises;
- a false promise must never silently look like checked behavior;
- internal callers may accept a strict trusted contract for speed.

Consequence:

Current modes have identical runtime behavior.

Future APIs must document what validation proves.

Trusted violations will be explicit caller errors.

## D12: Express policy as constraints

Status: accepted as design semantics, deferred in implementation.

FAST means:

```text
minimize L subject to M <= configured ceiling
```

BALANCED means:

```text
minimize M subject to L <= 1.10 * L_flat
```

COMPACT means:

```text
minimize M subject to L < L_radix
```

`L` is measured or calibrated lookup cost.

`M` is complete metadata footprint.

Reasoning:

- “five percent avoidable overhead” lacks a reference point;
- constraints can be tested;
- applications can choose a meaningful tradeoff.

Consequence:

The v0 policy enum reserves names.

Automatic policy calibration is not implemented.

## D13: Make representation transitions semantic-preserving

Status: accepted.

Transition shape is internal.

Lookup equivalence is public.

Current transitions include:

```text
EMPTY -> UNIFORM
EMPTY -> DENSE
DENSE -> UNIFORM
DENSE -> EMPTY
UNIFORM -> DENSE
ADAPTIVE -> FLAT
```

Reasoning:

- callers should not observe representation maintenance;
- tests can compare against a flat oracle;
- future copy/publish concurrency needs the same invariant.

Consequence:

Diagnostic APIs are unsuitable for application logic.

## D14: Permit internal shape changes on failed mutation

Status: accepted.

Failed operations preserve lookup results.

They need not restore allocation topology byte-for-byte.

Reasoning:

- rollback allocations add complexity and failure paths;
- an all-invalid DENSE table is semantically EMPTY;
- callers care about mappings, not hidden capacity.

Consequence:

Memory stats may change after some failed allocation sequences.

The fault suite checks semantic atomicity and leak freedom.

## D15: Count complete metadata

Status: accepted.

Reported requested bytes include the index object.

BOUNDED includes its descriptor array.

SPARSE includes its root and allocated leaves.

DENSE includes every secondary table.

FLAT includes every flat entry.

Reasoning:

- omitting coverage memory manufactures a false win;
- adaptive structures trade directory and representation costs;
- the central question is the combined footprint.

Consequence:

Requested bytes remain distinct from usable bytes, RSS, and VA reservation.

## D16: Maintain equal-semantic baselines

Status: accepted.

Gate 1 baselines return the same native-word handle.

They use the same granularity.

They use the same truth layout.

They use the same lookup address stream.

Reasoning:

- comparing different metadata richness is invalid;
- comparing different query streams is noisy;
- source-inspired structures are controls, not allocator integrations.

Consequence:

No “beats jemalloc” claim follows from the microbenchmark.

## D17: Gate ISA specialization on more than speed

Status: accepted for future work.

An ISA path needs a reproducible 3–5% meaningful improvement.

The decision also considers:

- `.text` bytes;
- dynamic instruction count;
- I-cache misses;
- dispatch cost;
- maintenance burden.

Reasoning:

- instruction count is not performance;
- a large backend can harm unrelated hot code;
- tiny wins may not generalize across microarchitectures.

Consequence:

Gate 1 contains no handwritten assembly.

`tools/source_metrics.py binary` records section and symbol sizes.

## D18: Prefer C masks and shifts over bitfields

Status: accepted.

Descriptor layout uses integer operations.

C implementation-defined bitfield layout is avoided.

Reasoning:

- endianness and compiler ABI affect bitfields;
- integer encoding is inspectable;
- future architecture ports need identical semantics.

Consequence:

Persistent formats must define byte order separately.

The internal descriptor is not persisted.

## D19: Use C17 as the current build floor

Status: provisional.

The implementation is written in portable C17 today.

It is compatible with a future C23 project baseline.

Reasoning:

- available compilers can build and sanitize C17 broadly;
- Gate 1 does not require a C23-only facility;
- experimental results should not depend on toolchain novelty.

Consequence:

CMake requests `c_std_17`.

A later move to C23 must name the required feature.

## D20: Keep benchmark claims layered

Status: accepted.

Claim levels are:

```text
0 synthetic standalone structure
1 allocator trace replay
2 allocator index substitution
3 end-to-end allocator workload
```

Reasoning:

- a microbenchmark isolates lookup but omits integration effects;
- trace replay adds realism but not allocator feedback;
- only end-to-end work supports broad production language.

Consequence:

Current results are Level 0 smoke evidence.

## D21: Freeze bounded indexes into a sparse-resident direct overlay

Status: provisional.

Linux-family targets may explicitly convert a bounded adaptive index to an
immutable VM-backed overlay. The conversion reserves one handle slot per
covered granule but commits/writes only pages intersecting DENSE regions.
Descriptors continue to encode EMPTY and inline UNIFORM regions.

The trusted lookup loads the descriptor and overlay entry independently, then
selects with a mask derived from the DENSE tag. The checked API retains range,
EMPTY, and invalid-tag validation.

Reasoning:

- mixed descriptor tags made the original branch unpredictable;
- the direct address removes a dependent dense-table load;
- virtual reservation can preserve direct-map arithmetic without forcing every
  overlay page resident;
- a frozen representation makes lifetime and mutation semantics explicit.

Consequence:

- conversion requires external exclusion and invalidates prior bounded views;
- insert and remove return `BUSY` after conversion;
- custom dense tables are freed during successful conversion;
- statistics separate directory bytes, page-rounded private representation
  bytes, and reserved virtual bytes;
- unsupported kernels return `UNSUPPORTED` without changing the index;
- the overlay remains a Level 0 experiment, not a novelty claim. snmalloc and
  other allocators already exploit virtual-address reservation for metadata.

The currently observed policy boundary is provisional: overlay is competitive
through roughly 50% DENSE regions on one x86-64 host, while fully dense input
uses the existing flat fallback. Real traces and other hardware must choose the
actual threshold.

## Rejected: separate saved and temporary indexes

Status: rejected.

Separate namespaces introduce cross-lookup and cache problems.

Heap lifetime is not a calling-convention register class.

One address index with explicit state is cleaner.

## Rejected: coroutines as reclamation semantics

Status: rejected.

Coroutines schedule work.

They do not prove memory safety.

Reclamation needs epochs, hazards, quiescence, references, or another proof.

## Rejected: assembly-first implementation

Status: rejected.

Assembly cannot rescue an unproven representation tradeoff.

Portable behavior, measurement, and compiler inspection come first.

## Rejected: allocator replacement in v0

Status: rejected.

MemX v0 is an index subsystem.

Allocation policy would confound the first experiment.

Allocator integration begins only after Gate 1 earns it.

## Review rule

Before changing an accepted decision, record:

1. the measured or correctness problem;
2. the proposed alternative;
3. compatibility impact;
4. benchmark impact;
5. test impact;
6. migration or removal plan.

Design decisions are constraints, not scripture.

Evidence may replace them.

Unmeasured ambition may not silently expand them.
