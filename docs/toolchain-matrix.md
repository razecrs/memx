# MemX local toolchain matrix

Validated on Fedora 44 x86-64. Every row first passed a header-free backend
probe, and `file` identified the expected format, ISA, word width, and
endianness. That probe demonstrates code-generation availability only; it is
not a MemX source build or a support claim.

C and C++ compiler drivers are installed for every GNU target in the table.
Strict full-project CMake compilation and linking is validated for host Linux
x86/x86-64, seven Bootlin GNU/Linux targets, all four Android ABIs, and Windows
x86/x86-64. On Linux/Android this includes both libraries, six C test
executables, the lookup/trace/heap benchmarks, trace replay, and allocator
workload. Python tool tests are host-only.
The experimental `memx_heap` target, heap tests, and heap benchmark are selected
only for Linux and Android targets. Windows deliberately skips that Unix
VM/pthread layer while continuing to build the portable MemX index.
The VM overlay implementation is selected by the Linux kernel ABI and therefore
compiled into all four Android executables. Windows executables compile the
portable `MEMX_ERROR_UNSUPPORTED` overlay stub; this is build coverage, not a
Windows overlay implementation.
Relocatable Bootlin 2025.08-1 glibc toolchains provide the seven complete
sysroots. Their published SHA-256 sidecars were verified before extraction.
Fedora glibc sysroots additionally compile all seventeen C translation units with
the strict GCC profile for AArch64, PowerPC64LE, and s390x, but their current
packages lack the target `libatomic_asneeded` needed by the GCC link specs.

Those three object-build claims require the mounted sysroots explicitly; the
bare cross drivers do not find the target C headers. The validated forms are:

```sh
aarch64-linux-gnu-gcc --sysroot=/usr/aarch64-redhat-linux/sys-root/fc44 -Iinclude -c src/memx.c
powerpc64le-linux-gnu-gcc --sysroot=/usr/ppc64le-redhat-linux/sys-root/fc44 -Iinclude -c src/memx.c
s390x-linux-gnu-gcc --sysroot=/usr/s390x-redhat-linux/sys-root/fc44 -Iinclude -c src/memx.c
```

The same `--sysroot` options are part of their local Compiler Explorer entries.

## Strict MemX project validation

"Link" means every CMake target produced an executable or archive with
warnings-as-errors enabled. "Object" means every one of the seventeen C
translation units compiled strictly, but the installed sysroot could not link.
None of these cross rows is a runtime or performance result.

| target / ABI | toolchain | strict result | artifact |
| --- | --- | --- | --- |
| Linux x86-64 | GCC 16.2.1, Clang 22.1.8 | link + native tests | ELF64 LE |
| Linux x86 | GCC 16.2.1 multilib | link + native tests | ELF32 LE |
| Linux ARMv7 hard-float | Bootlin GCC 14.3.0 glibc | full link | ELF32 LE EABI5 |
| Linux RISC-V ILP32D | Bootlin GCC 14.3.0 glibc | full link | ELF32 LE RV32 |
| Linux RISC-V LP64D | Bootlin GCC 14.3.0 glibc | full link | ELF64 LE RV64 |
| Linux MIPS32 | Bootlin GCC 14.3.0 glibc | full link | ELF32 BE MIPS32 |
| Linux MIPS64 N32 | Bootlin GCC 14.3.0 glibc | full link | ELF32 BE N32/MIPS64 |
| Linux PowerPC64 ELFv1 | Bootlin GCC 14.3.0 glibc | full link | ELF64 BE |
| Linux SPARC V9 | Bootlin GCC 14.3.0 glibc | full link | ELF64 BE |
| Linux AArch64 | Fedora GCC 16.1.1 sysroot | 17/17 objects | ELF64 LE |
| Linux PowerPC64LE ELFv2 | Fedora GCC 16.1.1 sysroot | 17/17 objects | ELF64 LE |
| Linux s390x | Fedora GCC 16.1.1 sysroot | 17/17 objects | ELF64 BE |
| Android ARM64/ARMv7/x86/x86-64 | NDK r29 Clang 21, API 35 | full link | ELF32/ELF64 |
| Windows x86/x86-64 | MinGW GCC 16.1.1 | full link | PE32/PE32+ |

The checked Bootlin archives and extracted relocatable toolchains live outside
the repository under:

```text
/home/raze/.local/share/memx-toolchains/bootlin
```

Source: [Bootlin pre-built toolchains](https://toolchains.bootlin.com/).

| target | compiler | emitted format |
| --- | --- | --- |
| Linux x86-64 | GCC 16.2.1, Clang 22.1.8 | ELF64 little-endian x86-64 |
| Linux x86 | GCC 16.2.1 multilib | ELF32 little-endian i386 |
| Linux AArch64 | GCC 16.1.1 | ELF64 little-endian AArch64 |
| Linux ARM32 | GCC 16.1.1 | ELF32 little-endian ARM EABI5 |
| Linux RISC-V 32 | GCC 16.1.1 | ELF32 little-endian RV32 |
| Linux RISC-V 64 | GCC 16.1.1 | ELF64 little-endian RV64 |
| Linux MIPS32 BE/LE | GCC 16.1.1 multilib | ELF32 MIPS64r2, both byte orders |
| Linux MIPS64 | GCC 16.1.1 | ELF64 big-endian MIPS64r2 |
| Linux PowerPC32 | GCC 16.1.1 multilib | ELF32 big-endian PowerPC |
| Linux PowerPC64 | GCC 16.1.1 | ELF64 big-endian ELFv1 |
| Linux PowerPC64LE | GCC 16.1.1 | ELF64 little-endian ELFv2 |
| Linux s390x | GCC 16.1.1 | ELF64 big-endian s390x |
| Linux LoongArch64 | GCC 16.1.1 | ELF64 little-endian LoongArch |
| Linux SPARC64 | GCC 16.1.1 | ELF64 big-endian SPARC V9 |
| Windows x86 | MinGW GCC 16.1.1 | i386 COFF |
| Windows x86-64 | MinGW GCC 16.1.1 | x86-64 COFF |
| Android ARM64 | NDK r29 Clang 21, API 35 | ELF64 little-endian AArch64 |
| Android ARMv7 | NDK r29 Clang 21, API 35 | ELF32 little-endian ARM EABI5 |
| Android x86 | NDK r29 Clang 21, API 35 | ELF32 little-endian i386 |
| Android x86-64 | NDK r29 Clang 21, API 35 | ELF64 little-endian x86-64 |

Android NDK location:

```text
/home/raze/Android/Sdk/ndk/29.0.14206865
```

Windows correctness can use Bottles 66.7 or Wine 11.0 Staging. Wine/Bottles
results must not be reported as native Windows performance. Native Windows or
real hardware is required for performance claims.

QEMU is explicitly excluded from the MemX execution and measurement path.
Cross-compilation is useful for compiler correctness, object inspection, and
CI build coverage; runtime correctness requires a native target, an attached
native OS image, Bottles/Wine where applicable, or real hardware.

## Local Compiler Explorer

Compiler Explorer is installed at:

```text
/home/raze/projects/lowlowlow/research/compiler-explorer
```

It is configured with all 22 compiler entries in
`etc/config/c.local.properties`. Start the C-only instance with:

```sh
cd /home/raze/projects/lowlowlow/research/compiler-explorer
make run-only EXTRA_ARGS='--language c'
```

Then open `http://localhost:10240/`. Its REST API was validated by compiling a
lookup primitive with the AArch64 compiler; GCC emitted `ubfx`, shifted `eor`,
and `ret` for the test expression.

## Architecture dispatch rule

MemX portable semantics remain in C. Build selection should compile exactly
one generic platform layer and only the ISA source files selected by the build
target. Runtime CPU-feature dispatch is appropriate only within one ISA (for
example x86-64 baseline versus BMI2), while compile-time target selection
separates x86, ARM, RISC-V, MIPS, PowerPC, and s390x. Unsupported combinations
must fail configuration rather than silently compiling the wrong assembly.
