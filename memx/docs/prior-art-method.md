# Prior-Art Comparison Method

## Purpose

MemX is not evaluated in an intellectual vacuum.

Production allocators already solve address-to-metadata lookup.

The local research set contains:

- jemalloc;
- mimalloc;
- TCMalloc;
- snmalloc.

The first two are primarily C codebases.

The latter two are primarily C++ codebases.

Language is descriptive context, not a performance explanation.

## What source counts mean

Lines of code measure repository size under one counting rule.

They do not directly measure algorithm complexity.

They do not directly measure executable size.

They do not directly measure maintenance cost.

They do not directly measure lookup latency.

They do not directly measure metadata footprint.

A repository may include tests, tools, docs, and platform ports.

A compact subsystem may live inside a very large allocator.

Generated code can dominate naïve counts.

Vendored dependencies can dominate naïve counts.

Blank lines can dominate physical counts.

Therefore every count needs its inclusion rule.

## Local count rule

The MemX first-party source metric includes:

- `.c`;
- `.h`;
- `.py`;
- `.md`;
- `.yml`;
- `.yaml`;
- `CMakeLists.txt`.

It excludes:

- `.git`;
- `research` clones;
- build directories;
- Python caches;
- test-runner caches.

It reports:

- files;
- physical lines;
- blank lines;
- nonblank lines;
- bytes;
- per-suffix totals;
- per-file totals.

Run it with:

```bash
python3 tools/source_metrics.py source .
```

## Competitor count rule

For a competitor report, record at least three scopes.

Scope A is the full checked-out repository.

Scope B is production library source.

Scope C is the address-index subsystem.

Use the same suffix rules within comparable scopes.

Explicitly list excluded vendored directories.

Explicitly list excluded generated directories.

Record the exact revision.

Record whether submodules were initialized.

Record whether generated files were present.

Never compare MemX first-party files with a competitor's full repository and
call the ratio algorithm complexity.

## Repository revision

Every local research checkout should record:

```text
remote URL
commit hash
commit date
dirty status
submodule status
```

Branch names are insufficient.

Default branches move.

Release names may be retagged in unusual workflows.

Commit hashes make inspection reproducible.

## Subsystem mapping

For each allocator, answer these questions.

What operation begins from an arbitrary pointer?

What address bits select the first structure?

How many dependent loads occur in the hot case?

Which loads are likely cache-resident?

What range validity is assumed?

What alignment is assumed?

What metadata is returned?

Is individual-object derivation a second step?

How is an unmapped address represented?

How is metadata lifetime protected?

How are large allocations treated?

How are tagged pointers normalized?

Which facts come from code?

Which facts come from comments or documentation?

Which facts are our inference?

## TCMalloc study target

Locate PageMap definitions and lookup calls.

Identify PageMap variants selected by address width.

Identify page shift and span semantics.

Identify root and leaf allocation policy.

Identify cache or fast-path layers around PageMap.

Identify whether lookup receives already-normalized page identifiers.

Do not reduce the system to one illustrative shift expression.

## jemalloc study target

Locate rtree definitions and extent metadata lookup.

Identify key-bit decomposition.

Identify leaf caching.

Identify dependent load depth.

Identify initialization and publication behavior.

Identify how safety checks differ between internal paths.

Identify metadata packed in leaf elements.

Do not claim rtree behavior from an obsolete release without labeling it.

## mimalloc study target

Locate segment and page derivation.

Identify alignment assumptions.

Identify when arithmetic replaces global lookup.

Identify huge-page or abandoned-segment exceptions.

Identify ownership and remote-free metadata.

Identify the fallback for pointers not fitting the common segment model.

Arithmetic derivation is an important baseline class of its own.

## snmalloc study target

Locate address-space metadata structures.

Identify compact entry encoding.

Identify ownership and message-queue interactions.

Identify how superslab and slab layout affect lookup.

Identify platform address-space assumptions.

Identify capability-platform paths when present.

Do not separate lookup from allocator layout when the design deliberately
couples them.

## Comparable semantic unit

MemX v0 returns an allocator-defined granule handle.

A competitor operation may return:

- a span pointer;
- an extent element;
- a page descriptor;
- a segment pointer;
- ownership metadata;
- a size class;
- several packed fields.

Latency comparisons require equal returned information.

If a competitor returns richer metadata, either:

- make MemX return an equivalent object;
- isolate the common sub-operation;
- label the semantic mismatch.

Do not ignore the mismatch.

## Comparable safety level

Checked lookup and allocator-internal lookup differ.

A production allocator may assume:

- the pointer lies in managed VA space;
- pointer tags were already removed;
- alignment was already checked;
- the caller holds a lifetime guard;
- the page is committed;
- metadata cannot disappear.

MemX comparisons must choose an equivalent path.

That is why benchmark names expose `assume_mapped`.

## Comparable memory accounting

Count all index-specific storage.

Include roots.

Include leaves.

Include caches.

Include descriptor arrays.

Include guard or epoch records when needed for lookup safety.

Include huge-allocation side structures.

Keep these categories separate:

```text
requested bytes
usable allocator bytes
resident bytes
reserved virtual bytes
```

Do not count allocator metadata unrelated to the lookup question unless MemX
would also need equivalent functionality.

## Comparable code footprint

Repository LOC is background context.

Hot-path footprint should use binary data.

Record:

- lookup symbol bytes;
- reachable helper bytes;
- read-only tables;
- dispatch code;
- initialization code;
- unwind or relocation data when deployment includes it.

For MemX:

```bash
python3 tools/source_metrics.py binary build-release/libmemx.a
```

Archive symbol sizes are not identical to linked executable footprint.

Link-time optimization may change both.

Record the exact binary being measured.

## Benchmark ladder

Use four comparison levels.

### Level 0

Standalone structures receive synthetic layouts.

This isolates lookup mechanisms.

It cannot establish allocator superiority.

### Level 1

Standalone structures replay allocator traces.

This tests layout and lifecycle realism.

It still omits allocator feedback.

### Level 2

MemX substitutes for an allocator index experimentally.

This exposes mutation ratios and call-site assumptions.

It may still use narrow microbenchmarks.

### Level 3

Complete allocator workloads run end to end.

This exposes cache interaction, RSS, throughput, and tail latency.

Only this level supports broad user-facing allocator claims.

## Claim language

Acceptable Level 0 language:

> On this synthetic uniform-region case, the MemX bounded unchecked lookup
> measured X while using Y requested metadata bytes.

Unacceptable Level 0 language:

> MemX beats jemalloc.

Acceptable negative language:

> On the 25% dense mixed case, the current dispatch failed the latency gate.

Negative results are part of the research output.

## Novelty review

Novelty is not established by different names.

UNIFORM resembles value compression.

DENSE resembles a local page map.

SPARSE directories resemble hierarchical page maps.

RUN resembles interval encoding.

Adaptive selection resembles data-dependent representation choice.

The research question concerns their allocator-specific combination and
tradeoff, not invention of these primitives.

Before any novelty claim, also study:

- adaptive radix trees;
- compressed radix trees;
- page tables;
- bitmap rank structures;
- succinct dictionaries;
- interval maps;
- random-access compression;
- Linux XArray;
- epoch reclamation;
- RCU;
- hazard pointers.

## Review checklist

Before publishing a competitor table:

1. pin every revision;
2. publish counting scripts;
3. list scope paths;
4. list exclusions;
5. identify generated code;
6. identify vendored code;
7. define semantic output;
8. define safety assumptions;
9. define memory category;
10. define hardware and compiler;
11. retain raw measurements;
12. separate fact from inference;
13. invite correction from maintainers;
14. correct mistakes publicly.

## Decision rule

Existing allocators are not targets to defeat by feature count.

They are mature evidence about useful tradeoffs.

MemX continues only if it adds a measurable Pareto point.

If an existing structure already dominates it, use the existing idea.

If allocator-derived arithmetic dominates generic indexing, document that.

If MemX wins only conditionally, describe the condition.

The goal is accurate systems knowledge, not a scoreboard.
