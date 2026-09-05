# Established math applied to MemX allocator work

This is an experiment plan and derivation, not a novelty or superiority claim.
The immediate implemented change is the large-allocation hash index. The other
ideas below remain candidates pending workload evidence.

## 1. Hashing and amortized resizing: applied now

For n live records and B buckets, load factor is alpha = n/B. Under a
well-distributed hashing model, separate chaining has expected O(1 + alpha)
lookup work. This does not prove a worst-case bound for our deterministic
address hash; deliberately colliding addresses can still require O(n) work.
The address mixer is the established MurmurHash3 64-bit finalizer; its
[reference source](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp)
is public domain. The choice is not an invention of MemX.

The table grows from B to 2B when full. Growing from the 64-bucket minimum
through powers of two moves a geometric series of records: 64 + 128 + ...,
whose total is less than twice the largest term. That is why repeated insertion
has amortized constant rehash work, assuming successful allocations and
well-distributed lookups. An individual resize still costs O(n) and holds the
heap mutex. Cornell's [amortized hash-table analysis](https://courses.cs.cornell.edu/cs3110/2021sp/textbook/eff/amortized_hash.html)
explains the geometric-growth principle.

Shrink at n <= B/4 and halve capacity. After a normal successful shrink, load
returns near 1/2, leaving a gap before the next growth. This hysteresis avoids
alternating a full grow and shrink on successive insert/free operations at one
boundary. A failed shrink leaves the old table; repeated allocation failures
are not covered by the normal successful-resize amortization argument.

Requested steady bucket space is B * sizeof(record pointer), in addition to
n existing records. During growth, old plus new arrays use 3B pointers; during
halving, 1.5B pointers. The last free returns bucket space to zero. With no
shrink failures, and above the minimum-size floor, B is less than 4n after
mutation. At 8,192 records on a 64-bit host, a full 8,192-bucket array costs
64 KiB. Record size is unchanged from the previous linked-list implementation.

The new report measures successful lookups, interior-pointer misses,
allocation, in-place shrink, and frees independently across population sizes.
Tiny populations can lose from hashing and an extra bucket allocation. Those
losses must remain visible alongside the large-population gains.

## 2. Size classes as quantization: next memory experiment

Let s be the requested payload and c(s) its selected size-class capacity.
Payload rounding waste is c(s)-s. For a measured request distribution p(s),
expected payload rounding is sum_s p(s) * (c(s)-s).

This gives an explicit objective for choosing classes. The allocator's current
power-of-two classes make indexing cheap but can waste almost as much payload
space as was requested immediately above a class boundary. A 4,097-byte request
uses the 8,192-byte payload class: 4,095 bytes of payload rounding before block
headers, span headers, unused span tails, and retained free capacity.

For a span of S bytes, header H, and aligned block stride d(c), capacity is
floor((S-H)/d(c)). Fully occupied span tail waste is
(S-H) - floor((S-H)/d(c)) * d(c). Optimizing payload rounding alone can worsen
tail waste or add bins/cache state; the benchmark must count all of them.

Candidate experiment: choose intermediate classes using measured histograms,
then compare requested/live bytes, committed/RSS peaks, class lookup cost,
cache footprint, and fragmentation after churn. Classify this as a hypothesis;
do not change classes until the workloads show the distribution it improves.
Wilson et al.'s [allocation survey](https://www.cs.hmc.edu/~oneill/gc-library/Wilson-Alloc-Survey-1995.pdf)
provides the established context for size segregation and fragmentation.

## 3. Sharding and batching: next contention experiment

For a refill batch b and fixed synchronization cost F, the amortized fixed
cost is approximately F/b per allocation from that batch. Larger b can reduce
lock traffic while retaining more objects in idle caches. It is a cost model,
not a latency guarantee or proof that the largest batch is best.

Mimalloc's [free-list sharding paper](https://www.microsoft.com/en-us/research/publication/mimalloc-free-list-sharding-in-action/)
describes page-local allocation, local-free and remote-free lists, with regular
maintenance driven by the allocation path. Our next comparable experiment
should measure lock contention and idle-owner retention in producer/consumer
workloads before choosing sharding or bounded queue-drain policies.

## Acceptance against mimalloc

Use the same workload source, request sequence, payload touches, realloc
preservation rules, initialization boundaries and operation count. Report the
MemX checked API separately from its conventional unchecked API. Verify the
loaded mimalloc binary and pin its source revision.

The existing mixed small-object workload reaches only 8,192 bytes; it does not
exercise the new large-allocation index. It is a regression/comparator check,
not evidence that hashing sped up that workload. Separate large-object and
over-aligned traces are needed for allocator-level comparisons of this change.
Beating mimalloc substantially remains an open goal. A size-query improvement
alone cannot establish an allocator-level win.
