# From runtime primitives to a Discord library

Ambition: beat every competitor under every condition we test.

Status: design and competitor map, researched 2026-09-05. None of these tiers is
claimed implemented here. MemX lives independently in `../memx/`. Initial
working assumption: handwritten x86-64 Linux assembly, suggested by `bot.asm`;
language and assembler selection remain open. No bot source was supplied here.

Priority: finish the current MemX work before implementing this stack.

## Accepted future architecture

Keep OS services behind layer 0: memory mapping, sockets, connection setup,
I/O, close, nonblocking mode, event delivery, monotonic time, entropy, error
translation, and discovery of DNS configuration and certificate trust stores.
Linux uses its syscall ABI; macOS uses supported system-library entry points;
Windows uses documented memory, Winsock, completion, clock, and crypto APIs.
Raw syscall numbers in the initial sketch apply specifically to Linux x86-64;
they are not portable ISA constants. Raw syscalls exist on other OSes, but are
not the interface this project commits to supporting.

Choose an operation/completion contract before implementing the event loop.
The readiness backends (epoll/kqueue) drive nonblocking I/O until completion;
an IOCP backend reports OS completions. Specify buffer lifetime, cancellation,
partial transfers, and exactly-once terminal notification at that boundary.
This is a design requirement to prove with tests, not an implemented adapter.

Socket-address construction stays inside the platform boundary; portable code
uses a logical IP address and port. BSD/macOS length/family layouts differ from
Linux. Windows uses the Winsock `SOCKET` type (not a POSIX fd), initialization,
socket-specific close/error operations, and separate errors for non-socket OS
services. User-space environment/startup and trust/DNS configuration also
require platform adapters even when parsing itself is portable.
Microsoft documents the [IOCP completion model](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
and the [Winsock SOCKET type](https://learn.microsoft.com/en-us/windows/win32/winsock/socket-data-type-2).

Above that boundary the intended layers are memory/string primitives;
checked conversions, formatting and allocation; logical IP parsing/formatting;
DNS; TLS 1.3 including crypto and X.509 validation; HTTP/1.1; WebSocket; then
JSON and Discord REST/Gateway behavior. Portable semantics are shared across
targets; handwritten assembly implementations still require separate ISA/ABI
backends. A single assembly implementation is not portable machine code.

## The six foundation tiers

| Tier | Own implementation | What it enables | Comparison targets |
|---|---|---|---|
| 1 | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strcmp`, `strchr`, `strstr`; add bounded variants needed by parsers | Memory and string primitives | [glibc](https://www.gnu.org/software/libc/), [musl](https://musl.libc.org/), [LLVM libc](https://libc.llvm.org/); [Arm optimized routines](https://github.com/ARM-software/optimized-routines) for later ARM builds |
| 2 | Decimal parsing, integer formatting, `htons`/`htonl` and inverse conversions | Checked port parsing and wire byte order | libc `strtol`/`strtoul` and formatting; [fmt](https://github.com/fmtlib/fmt) for matching integer-format operations |
| 3 | Target-correct `sockaddr_in`/`sockaddr_in6`, `inet_pton`/`inet_ntop`; socket/connect/close wrappers | TCP connection to numeric IPv4/IPv6 addresses | glibc and musl address conversion; same-kernel socket wrappers |
| 4 | Process startup/environment access, `getenv`, explicitly scoped `sprintf` compatibility plus bounded formatting | Build the existing bot against our own runtime once its complete symbol dependencies are inventoried | glibc/musl `getenv` and matching formatting subsets; fmt where semantics match |
| 5 | `clock_gettime`, `epoll_*`, `fcntl`, nonblocking connection state and deadlines | An event loop that tolerates partial I/O and stalled peers | libc wrappers for wrapper overhead; [libuv](https://libuv.org/) and [libevent](https://libevent.org/) for event-loop workloads |
| 6 | `sendto`/`recvfrom`, DNS stub resolver, address selection and `net_dial(host:port)` | Resolve a hostname and establish TCP | [c-ares](https://c-ares.org/), musl/glibc `getaddrinfo` under explicitly matched resolver policy |

These are comparator projects, not bundled runtime dependencies or claims that
every function exists on every LLVM libc target. Pin source revisions and
actually build each selected baseline before collecting comparison numbers.
LLVM libc publishes per-function/platform coverage; c-ares is specifically an
asynchronous DNS resolver. Do not compare an asynchronous resolver against a
blocking call without reporting the concurrency and caching policy.

`atoi` compatibility is separate from safe port parsing: ports need a bounded
parser with explicit overflow/trailing-input errors and a 0..65535 range check.
Formatting must define its supported conversions, widths, precision, buffer
capacity, and errors. A limited formatter must not claim full `sprintf` parity.
Float conversion is a later feature: [Ryu](https://github.com/ulfjack/ryu) and
[fast_float](https://github.com/fastfloat/fast_float) are relevant float-formatting
and parsing comparators, not substitutes for the integer conversion baseline.

Tier 3 structs and tier 5 syscall numbers belong to the target ABI. Keep CPU
selection and OS selection separate. Each later target must pass its own ABI
tests before being marked supported. No empty architecture directories or
untested support macros are needed at this stage.

## What comes after TCP

The six tiers reach a TCP socket. Discord additionally requires secure HTTP and
WebSocket connections, as documented in its [API reference](https://docs.discord.com/developers/reference).
The remaining implementation order is:

1. TLS records, handshake, authenticated encryption, secure randomness,
   certificate chain/hostname validation, and trust-store handling. Use protocol
   test vectors and compare interoperability with [OpenSSL](https://github.com/openssl/openssl)
   and [BearSSL](https://bearssl.org/) within their matching protocol support.
   An authored TLS implementation needs a separate specification and security
   review before handling real credentials. Disabling certificate validation
   does not satisfy the TLS milestone.
2. HTTP request/response framing, bounded incremental JSON parsing, and
   WebSocket upgrade/framing, masking, fragmentation, ping/pong, and closure.
   Test every possible split point and multiple frames per read.
3. Discord REST routing, authentication, response decoding, and rate-limit
   buckets. Respect response-driven limits and `retry_after`; test with a local
   scripted server rather than generating load against Discord. See the official
   [rate-limit contract](https://docs.discord.com/developers/topics/rate-limits).
4. Gateway Hello/Identify/Ready, intents, sequence tracking, heartbeats/ACKs,
   reconnect/resume, and session limits. The [Gateway specification](https://docs.discord.com/developers/events/gateway)
   defines the state transitions. Compression, voice, sharding, and broad
   endpoint coverage get separate milestones after the first reliable session.
5. One bot integration demonstrating a reconnectable Gateway session and one
   REST action, with explicit user-provided bot credentials and a test guild.
   The benchmark suite uses local fixtures and never embeds tokens.

## Discord-level competitors

| Project | Language | Use as a comparator |
|---|---|---|
| [Concord](https://github.com/Cogmasters/concord) | C99 | Closest C library comparison: REST, asynchronous dispatch, Gateway behavior |
| [D++ / DPP](https://github.com/brainboxdotcc/DPP) | C++ | C++ bot-library throughput, memory, event and connection handling |
| [Sleepy Discord](https://github.com/yourWaifu/sleepy-discord) | C++ | Another API/library design and interoperability reference |
| [discord.js](https://discord.js.org/) | JavaScript | Application-level behavior and developer-facing feature coverage |
| [discord.py](https://discordpy.readthedocs.io/en/stable/) | Python | Application-level behavior, reconnect handling, and feature coverage |

Language runtime, TLS backend, cache policy, parsing work, event coverage, and
intent configuration must be recorded. Comparing a minimal event parser with a
library that builds and caches complete guild objects would not establish a
like-for-like speed win. No current relative speed or maintenance ranking is
claimed for these projects.

## Evidence required to advance

- Tier 1: randomized differential tests; every alignment and vector-tail
  boundary; guard pages; unsigned comparison semantics; overlap in both
  directions for `memmove`; early and late NUL bytes; empty needles.
- Tiers 2–4: integer extrema/overflow, invalid text, endian round trips, IPv6
  compression/embedded IPv4, undersized output buffers, missing environment
  variables, and documented formatting-subset behavior.
- Tier 5: short reads/writes, `EINTR`, `EAGAIN`, connection errors, EOF,
  descriptor reuse, deadline expiry, cancellation and no leaked descriptors.
- Tier 6: deterministic local DNS fixtures for A/AAAA, NXDOMAIN, CNAME chains,
  truncation/TCP fallback, compressed-name loops, spoofed/mismatched replies,
  timeout/retry policy, TTL expiry, and bracketed IPv6 host:port parsing.
- Runtime/linkage: define `_start`, stack alignment, environment lifetime,
  allocation, locks, error convention, and any compiler-runtime helpers.
  Verify ELF dynamic dependencies and undefined symbols with `readelf`/`nm`;
  the test harness may link libc, while the delivered libc-free binary must
  satisfy the declared runtime-dependency contract.
- Benchmarks: matched semantics, reproducible seeds, multiple independent runs,
  latency distributions, throughput, resident memory and executable text size.
  Record both wins and losses in JSON. CPU measurements require native hardware;
  emulation remains useful for ABI and correctness checks.

The current MemX heap calls libc allocation functions and pthreads internally.
Its integration into this runtime requires an explicit dependency-removal
milestone; moving the folder does not make it libc-free.

First implementation milestone: choose the initial assembler/ABI, define the
runtime symbol prefix and build contract, then implement and differentially
test tier 1 with a minimal libc-free executable. The dependency graph above is
the development plan; the one-line ambition is not a completed benchmark claim.
