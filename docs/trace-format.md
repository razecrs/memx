# MemX Trace Format v1

## Scope

The trace format records logical metadata-index activity.

It does not record allocator payload bytes.

It does not claim to capture a complete allocator execution.

It is designed for deterministic replay into index candidates.

Version 1 includes lifecycle vocabulary before concurrency is implemented.

RETIRE and QUIESCENT are trace facts, not public MemX v0 mutation APIs.

## Encoding

The file is UTF-8-compatible ASCII text.

Line endings may be LF or CRLF.

Leading and trailing whitespace is ignored around fields.

Empty lines are ignored.

Lines whose first non-space character is `#` are comments.

Inline comments are not supported.

Each non-comment line must fit in 1,023 bytes plus the terminating null byte.

Longer lines are rejected.

## File grammar

A trace has this order:

```text
magic
headers
column declaration
events
```

The magic line is:

```text
MEMX_TRACE
```

The required headers are:

```text
version=1
region_shift=21
granule_shift=12
```

Unknown header names are rejected.

The column declaration is exact:

```text
sequence,thread,operation,address,size,handle
```

An event has exactly six comma-separated fields.

Quoted CSV fields are not supported in version 1.

## Numeric syntax

Numeric fields use the C integer conventions accepted by `strtoull`.

Decimal is accepted.

Hexadecimal with `0x` is accepted.

Octal syntax may be accepted by the host C library.

Negative values are not part of the format contract.

Values must fit their destination type.

`sequence` is `uint64_t`.

`thread` is `uint32_t`.

`address` is `uintptr_t`.

`size` is `size_t`.

`handle` is `memx_handle_t`.

A trace written on a wider host may not load on a narrower host.

Address rebasing should happen before such a replay.

## Shift headers

`granule_shift` defines the mutation alignment.

The granule size is:

```text
1 << granule_shift
```

`region_shift` defines the experimental region size.

The region size is:

```text
1 << region_shift
```

The required relationship is:

```text
0 <= granule_shift <= region_shift < uintptr width
```

The C implementation rejects shifts outside that relationship.

## Event ordering

Sequence values must be strictly increasing.

They do not need to begin at zero or one.

They do not need to be contiguous.

File order is replay order.

Sequence values make accidental reorderings detectable.

Wall-clock timestamps are deliberately absent from version 1.

## INSERT

Syntax:

```text
sequence,thread,INSERT,address,size,handle
```

Requirements:

- address is granule-aligned;
- size is nonzero;
- size is granule-aligned;
- address plus size does not overflow;
- handle is not `MEMX_HANDLE_INVALID`;
- every covered granule is currently unmapped.

Effect:

```text
EMPTY -> ACTIVE(handle)
```

INSERT is atomic in the logical trace model.

Partial overlap is invalid.

Reusing an address after REMOVE is valid.

## LOOKUP

Syntax:

```text
sequence,thread,LOOKUP,address,0,expected_handle
```

The address need not be granule-aligned.

`UINTPTR_MAX` is rejected because the diagnostic coverage interval would
require an unrepresentable exclusive upper bound.

Size must be zero.

The handle is the expected result.

An expected miss uses:

```text
MEMX_HANDLE_INVALID
```

The lifecycle validator compares the expected handle with its oracle.

A RETIRED mapping remains visible to LOOKUP.

This models deferred reclamation.

The handle disappears only after REMOVE.

## RETIRE

Syntax:

```text
sequence,thread,RETIRE,address,size,handle
```

Requirements:

- address is granule-aligned;
- size is nonzero and granule-aligned;
- the entire range carries the supplied handle;
- every granule in the range is ACTIVE;
- no granule in the range is already RETIRED.

Effect:

```text
ACTIVE(handle) -> RETIRED(handle)
```

RETIRE does not remove lookup visibility.

It records logical deallocation before safe reuse.

Version 1 permits a range to be removed without a preceding RETIRE.

That supports allocators without deferred reclamation.

## REMOVE

Syntax:

```text
sequence,thread,REMOVE,address,size,handle
```

Requirements:

- address is granule-aligned;
- size is nonzero and granule-aligned;
- the entire range is mapped;
- the entire range carries the supplied handle.

The range may be ACTIVE.

The range may be RETIRED.

Effect:

```text
ACTIVE(handle)  -> EMPTY
RETIRED(handle) -> EMPTY
```

After REMOVE, LOOKUP expects invalid until another INSERT.

## QUIESCENT

Syntax:

```text
sequence,thread,QUIESCENT,0,0,handle
```

Address and size must be zero.

Version 1 does not interpret the handle field.

Writers should emit zero.

QUIESCENT records a thread lifecycle observation.

It does not itself change any mapped granule.

Future reclamation experiments may interpret the event.

The v1 lifecycle validator treats it as a no-op.

## Structural validation

`memx_trace_validate` checks shape and field constraints.

It does not build allocation state.

A structurally valid trace may still contain an overlapping INSERT.

A structurally valid trace may still contain a false LOOKUP expectation.

A structurally valid trace may still REMOVE an unmapped range.

This separation makes parser failures distinguishable from workload failures.

## Lifecycle validation

`memx_trace_validate_lifecycle` first runs structural validation.

It then constructs an independent granule hash map.

The map stores:

- aligned granule address;
- handle;
- live/tombstone state;
- retired flag.

Hash-map tombstones permit address reuse after REMOVE.

The validator sizes the table from potential INSERT granules.

It caps validation at 67,108,864 inserted granules.

The cap protects diagnostic tools from unreasonable allocation requests.

Exceeding the cap returns `MEMX_TRACE_ERROR_MEMORY`.

The validator reports the zero-based failing event index.

## Loader ownership contract

Before calling `memx_trace_read_file`, initialize the destination:

```c
memx_trace_t trace;
memx_trace_init(&trace);
```

On success, any prior event buffer in the initialized destination is freed.

The loaded trace replaces it.

On failure, the destination remains unchanged.

Destroy the destination when finished:

```c
memx_trace_destroy(&trace);
```

Passing an uninitialized destination is invalid caller behavior.

This rule exists because the loader supports replacing an existing trace.

## Writer behavior

`memx_trace_write_file` requires lifecycle validity.

It emits canonical operation names.

It emits addresses and handles in hexadecimal.

It emits sequence, thread, and size in decimal.

It emits required headers in a stable order.

Comments from an input file are not preserved.

Whitespace formatting from an input file is not preserved.

The semantic event sequence is preserved.

## Statistics

`memx_trace_calculate_stats` requires lifecycle validity.

It reports operation counts.

It reports distinct logical threads.

It reports minimum referenced address.

It reports maximum referenced address, exclusive.

For LOOKUP, the referenced interval is one byte.

It reports peak live bytes.

It reports final live bytes.

RETIRE does not decrease live bytes.

REMOVE decreases live bytes.

This treats retired storage as still occupying allocator space.

## Canonical example

```text
MEMX_TRACE
version=1
region_shift=16
granule_shift=12
sequence,thread,operation,address,size,handle
1,0,INSERT,0x10000,8192,0x7
2,0,LOOKUP,0x10011,0,0x7
3,1,LOOKUP,0x11022,0,0x7
4,0,RETIRE,0x10000,8192,0x7
5,1,LOOKUP,0x10080,0,0x7
6,0,QUIESCENT,0x0,0,0x0
7,1,QUIESCENT,0x0,0,0x0
8,0,REMOVE,0x10000,8192,0x7
9,1,LOOKUP,0x10080,0,0xffffffffffffffff
```

The final sentinel spelling is host-width dependent.

Trace generators should derive it from `uintptr_t` width when portability
matters.

## Generator

`tools/generate_trace.py` creates deterministic synthetic traces.

Its defaults are for experiments, not representative allocator claims.

Record all generator arguments with a result.

Never describe a generated trace as a production workload.

Useful controlled dimensions include:

- thread count;
- live allocation target;
- allocation size distribution;
- lookup-to-mutation ratio;
- retirement delay;
- address fragmentation;
- handle repetition;
- random seed.

## Replay

`memx_trace_replay` loads and validates a trace. Its final and peak memory
columns use `page_accounted_bytes`, the same conservative backing-page category
used by the synthetic benchmark gate; they are not RSS measurements.

It derives a bounded managed range from referenced addresses.

It replays INSERT and REMOVE into MemX.

It checks every LOOKUP expectation.

It records final metadata bytes.

It records peak metadata bytes.

It reports operation counts.

RETIRE and QUIESCENT remain no-ops for the Gate 1 index.

Replay timing is diagnostic and is labeled `instrumented_ns_per_lookup`.

Per-event clock calls can dominate very small lookups.

Publication lookup latency comes from the dedicated benchmark harness.

## Address privacy

Real traces may expose virtual address layout.

Collectors should offer deterministic rebasing.

Rebasing must preserve:

- granule alignment;
- region boundaries when required by the study;
- gaps;
- overlap relationships;
- ordering.

Payload memory must never be written to the trace.

Handles should be remapped to opaque equivalence-class identifiers.

Thread identifiers may also be compacted.

## Versioning rules

A semantic change requires a new version.

Adding an optional comment convention does not.

Changing required columns does.

Changing RETIRE visibility does.

Changing sentinel interpretation does.

Changing numeric widths does.

A reader must reject unsupported versions.

It must not guess how to interpret them.

## Known v1 limitations

Version 1 has no timestamp.

It has no allocation call-site identity.

It has no NUMA node.

It has no explicit dependency chain between lookups.

It has no allocator layout hint fields.

It has no generation identifier separate from handle.

It has no explicit reclamation epoch.

It has no binary encoding.

It has no compression.

These omissions are deliberate for Gate 1.

Add fields only when a measured experiment requires them.
