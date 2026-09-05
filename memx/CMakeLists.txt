cmake_minimum_required(VERSION 3.20)

project(memx VERSION 0.1.0 LANGUAGES C)

option(MEMX_BUILD_TESTS "Build MemX tests" ON)
option(MEMX_BUILD_BENCHMARKS "Build MemX benchmarks" ON)
option(MEMX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(MEMX_STRICT_WARNINGS "Enable the strict portable warning profile" ON)
option(MEMX_ENABLE_GCC_ANALYZER "Run GCC's interprocedural static analyzer" OFF)
option(MEMX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MEMX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MEMX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MEMX_BUILD_FUZZERS "Build libFuzzer targets with Clang" OFF)
option(MEMX_BUILD_ALLOCATOR
    "Build the experimental Linux/Android allocator layer" ON)

function(memx_target_warnings target)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(MEMX_STRICT_WARNINGS)
            target_compile_options(${target} PRIVATE
                -Wsign-conversion
                -Wcast-align=strict
                -Wcast-qual
                -Wstrict-prototypes
                -Wmissing-prototypes
                -Wold-style-definition
                -Wundef
                -Wformat=2
                -Wformat-overflow=2
                -Wformat-truncation=2
                -Wwrite-strings
                -Wdouble-promotion
                -Wnull-dereference
                -Wduplicated-cond
                -Wduplicated-branches
                -Wlogical-op
                -Walloca
                -Wvla
                -Wswitch-enum
                -Wswitch-default
                -Wpointer-arith
                -Wbad-function-cast)
        endif()
        if(MEMX_ENABLE_GCC_ANALYZER)
            target_compile_options(${target} PRIVATE -fanalyzer)
        endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
        if(MEMX_STRICT_WARNINGS)
            target_compile_options(${target} PRIVATE
                -Weverything
                # Intentional C17/raw-array design, not suppressed findings.
                -Wno-pre-c11-compat
                -Wno-unsafe-buffer-usage
                # ABI padding is measured; rearranging public structs is not
                # a correctness improvement.
                -Wno-padded
                # Defaults diagnose corrupted/future enum values.
                -Wno-covered-switch-default)
            # Introduced in Clang 22. C permits implicit conversion from
            # allocation functions' void pointer results.
            if(CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 22)
                target_compile_options(${target} PRIVATE
                    -Wno-implicit-void-ptr-cast)
            endif()
        else()
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()
    endif()
    if(MEMX_WARNINGS_AS_ERRORS
        AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

if(MEMX_ENABLE_ASAN AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(MEMX_ENABLE_UBSAN AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined)
endif()

if(MEMX_ENABLE_TSAN AND (MEMX_ENABLE_ASAN OR MEMX_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer requires a separate build")
endif()

if(MEMX_ENABLE_TSAN AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

add_library(memx src/memx.c)
add_library(memx::memx ALIAS memx)

target_include_directories(memx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(memx PUBLIC c_std_17)

memx_target_warnings(memx)

if(MEMX_BUILD_ALLOCATOR AND CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    find_package(Threads REQUIRED)
    add_library(memx_heap
        src/allocator/heap.c
        src/allocator/os_linux.c)
    add_library(memx::heap ALIAS memx_heap)
    target_include_directories(memx_heap
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE src/allocator)
    target_compile_features(memx_heap PUBLIC c_std_17)
    target_link_libraries(memx_heap PUBLIC memx PRIVATE Threads::Threads)
    memx_target_warnings(memx_heap)
endif()

if(MEMX_BUILD_TESTS)
    enable_testing()
    add_executable(memx_test tests/test_memx.c)
    target_link_libraries(memx_test PRIVATE memx)
    target_compile_features(memx_test PRIVATE c_std_17)
    memx_target_warnings(memx_test)
    add_test(NAME memx_test COMMAND memx_test)

    add_executable(memx_baseline_test
        tests/test_baselines.c
        bench/baselines.c)
    target_include_directories(memx_baseline_test PRIVATE bench)
    target_link_libraries(memx_baseline_test PRIVATE memx)
    target_compile_features(memx_baseline_test PRIVATE c_std_17)
    memx_target_warnings(memx_baseline_test)
    add_test(NAME memx_baseline_test COMMAND memx_baseline_test)

    add_executable(memx_trace_test
        tests/test_trace.c
        bench/trace.c)
    target_include_directories(memx_trace_test PRIVATE bench)
    target_link_libraries(memx_trace_test PRIVATE memx)
    target_compile_features(memx_trace_test PRIVATE c_std_17)
    memx_target_warnings(memx_trace_test)
    add_test(NAME memx_trace_test COMMAND memx_trace_test)

    add_executable(memx_exhaustive_test tests/test_exhaustive.c)
    target_link_libraries(memx_exhaustive_test PRIVATE memx)
    target_compile_features(memx_exhaustive_test PRIVATE c_std_17)
    memx_target_warnings(memx_exhaustive_test)
    add_test(NAME memx_exhaustive_test COMMAND memx_exhaustive_test)

    add_executable(memx_fault_test tests/test_faults.c)
    target_link_libraries(memx_fault_test PRIVATE memx)
    target_compile_features(memx_fault_test PRIVATE c_std_17)
    memx_target_warnings(memx_fault_test)
    add_test(NAME memx_fault_test COMMAND memx_fault_test)

    if(TARGET memx_heap)
        add_executable(memx_heap_test tests/test_allocator.c)
        target_link_libraries(memx_heap_test PRIVATE memx_heap Threads::Threads)
        target_compile_features(memx_heap_test PRIVATE c_std_17)
        memx_target_warnings(memx_heap_test)
        add_test(NAME memx_heap_test COMMAND memx_heap_test)
        add_executable(memx_cache_bounds_test
            tests/test_cache_bounds.c src/allocator/os_linux.c)
        target_link_libraries(memx_cache_bounds_test PRIVATE memx Threads::Threads)
        target_compile_features(memx_cache_bounds_test PRIVATE c_std_17)
        memx_target_warnings(memx_cache_bounds_test)
        add_test(NAME memx_cache_bounds_test COMMAND memx_cache_bounds_test)

        add_executable(memx_thp_concurrency_test tests/test_thp_concurrency.c)
        target_link_libraries(memx_thp_concurrency_test PRIVATE memx_heap Threads::Threads)
        target_compile_features(memx_thp_concurrency_test PRIVATE c_std_17)
        memx_target_warnings(memx_thp_concurrency_test)
        add_test(NAME memx_thp_concurrency_test COMMAND memx_thp_concurrency_test)

        add_executable(memx_malloc_shim_test tests/test_malloc_shim.c)
        target_link_libraries(memx_malloc_shim_test PRIVATE memx_heap Threads::Threads)
        target_compile_features(memx_malloc_shim_test PRIVATE c_std_17)
        memx_target_warnings(memx_malloc_shim_test)
        add_test(NAME memx_malloc_shim_test COMMAND memx_malloc_shim_test)
        set_tests_properties(memx_cache_bounds_test memx_thp_concurrency_test
            memx_malloc_shim_test PROPERTIES TIMEOUT 120)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
            AND NOT BUILD_SHARED_LIBS)
            add_executable(memx_large_fault_test tests/test_large_faults.c)
            target_link_libraries(memx_large_fault_test PRIVATE memx_heap Threads::Threads)
            target_link_options(memx_large_fault_test PRIVATE -Wl,--wrap=calloc)
            target_compile_features(memx_large_fault_test PRIVATE c_std_17)
            memx_target_warnings(memx_large_fault_test)
            add_test(NAME memx_large_fault_test COMMAND memx_large_fault_test)
        endif()
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME memx_tool_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tools.py)
    endif()
endif()

if(MEMX_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MEMX_BUILD_FUZZERS requires Clang")
    endif()
    add_executable(memx_fuzz fuzz/fuzz_memx.c)
    target_link_libraries(memx_fuzz PRIVATE memx)
    memx_target_warnings(memx_fuzz)
    target_compile_options(memx_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(memx_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    if(TARGET memx_heap)
        add_executable(memx_heap_fuzz fuzz/fuzz_allocator.c)
        target_link_libraries(memx_heap_fuzz PRIVATE memx_heap Threads::Threads)
        memx_target_warnings(memx_heap_fuzz)
        target_compile_options(memx_heap_fuzz PRIVATE
            -fsanitize=fuzzer,address,undefined)
        target_link_options(memx_heap_fuzz PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endif()
endif()

if(MEMX_BUILD_BENCHMARKS)
    add_executable(memx_allocator_workload bench/allocator_workload.c)
    target_compile_features(memx_allocator_workload PRIVATE c_std_17)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(memx_allocator_workload PRIVATE -O3)
    endif()
    memx_target_warnings(memx_allocator_workload)

    if(TARGET memx_heap)
        add_executable(memx_large_heap_bench bench/large_heap_bench.c)
        target_link_libraries(memx_large_heap_bench PRIVATE memx_heap Threads::Threads)
        target_compile_features(memx_large_heap_bench PRIVATE c_std_17)
        memx_target_warnings(memx_large_heap_bench)

        add_executable(memx_heap_workload bench/allocator_workload.c)
        target_compile_definitions(memx_heap_workload PRIVATE
            MEMX_WORKLOAD_USE_MEMX_HEAP=1)
        target_link_libraries(memx_heap_workload PRIVATE
            memx_heap Threads::Threads)
        target_compile_features(memx_heap_workload PRIVATE c_std_17)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(memx_heap_workload PRIVATE -O3)
        endif()
        memx_target_warnings(memx_heap_workload)

        add_executable(memx_heap_workload_checked bench/allocator_workload.c)
        target_compile_definitions(memx_heap_workload_checked PRIVATE
            MEMX_WORKLOAD_USE_MEMX_HEAP=1
            MEMX_WORKLOAD_USE_CHECKED_API=1)
        target_link_libraries(memx_heap_workload_checked PRIVATE
            memx_heap Threads::Threads)
        target_compile_features(memx_heap_workload_checked PRIVATE c_std_17)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(memx_heap_workload_checked PRIVATE -O3)
        endif()
        memx_target_warnings(memx_heap_workload_checked)

        # Multi-shape single-threaded comparison. The system build exists so a
        # competitor can be LD_PRELOADed into the identical binary.
        add_executable(memx_pattern_bench bench/allocator_patterns.c)
        target_compile_definitions(memx_pattern_bench PRIVATE
            MEMX_PATTERNS_USE_MEMX_HEAP=1)
        target_link_libraries(memx_pattern_bench PRIVATE
            memx_heap Threads::Threads)
        target_compile_features(memx_pattern_bench PRIVATE c_std_17)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(memx_pattern_bench PRIVATE -O3)
        endif()
        memx_target_warnings(memx_pattern_bench)

        add_executable(memx_pattern_bench_system bench/allocator_patterns.c)
        target_compile_features(memx_pattern_bench_system PRIVATE c_std_17)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(memx_pattern_bench_system PRIVATE -O3)
        endif()
        memx_target_warnings(memx_pattern_bench_system)

        # Benchmark-only malloc interposer, so MemX can be measured through the
        # same LD_PRELOAD path as snmalloc and mimalloc.
        if(UNIX AND NOT APPLE)
            # Compiled from sources rather than linked against the static
            # library, so the shared object stays position independent without
            # forcing PIC on the library targets.
            add_library(memx_malloc_shim SHARED
                bench/memx_malloc_shim.c
                src/memx.c
                src/allocator/heap.c
                src/allocator/os_linux.c)
            target_include_directories(memx_malloc_shim
                PRIVATE include src/allocator)
            target_link_libraries(memx_malloc_shim PRIVATE Threads::Threads)
            target_compile_features(memx_malloc_shim PRIVATE c_std_17)
            set_target_properties(memx_malloc_shim PROPERTIES
                POSITION_INDEPENDENT_CODE ON)
            if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(memx_malloc_shim PRIVATE -O3)
            endif()
            memx_target_warnings(memx_malloc_shim)
            # The white-box shim test above works with sanitizers. Actual
            # LD_PRELOAD interposition must not replace sanitizer allocators.
            if(MEMX_BUILD_TESTS AND NOT MEMX_ENABLE_ASAN
                AND NOT MEMX_ENABLE_UBSAN AND NOT MEMX_ENABLE_TSAN
                AND NOT CMAKE_CROSSCOMPILING)
                add_test(NAME memx_compare_allocators_test
                    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compare_allocators.sh
                        $<TARGET_FILE_DIR:memx_pattern_bench_system>)
                set_tests_properties(memx_compare_allocators_test PROPERTIES TIMEOUT 120)
            endif()
        endif()

        add_executable(memx_heap_bench bench/heap_bench.c)
        target_link_libraries(memx_heap_bench PRIVATE memx_heap Threads::Threads)
        target_compile_features(memx_heap_bench PRIVATE c_std_17)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(memx_heap_bench PRIVATE -O3)
        endif()
        memx_target_warnings(memx_heap_bench)
    endif()

    add_executable(memx_bench
        bench/memx_bench.c
        bench/baselines.c)
    target_include_directories(memx_bench PRIVATE bench)
    target_link_libraries(memx_bench PRIVATE memx)
    target_compile_features(memx_bench PRIVATE c_std_17)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(memx_bench PRIVATE -O3)
    endif()
    memx_target_warnings(memx_bench)

    add_executable(memx_trace_replay
        bench/memx_trace_replay.c
        bench/trace.c)
    target_include_directories(memx_trace_replay PRIVATE bench)
    target_link_libraries(memx_trace_replay PRIVATE memx)
    target_compile_features(memx_trace_replay PRIVATE c_std_17)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(memx_trace_replay PRIVATE -O3)
    endif()
    memx_target_warnings(memx_trace_replay)
endif()
