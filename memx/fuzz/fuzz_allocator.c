#include "memx/allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    memx_heap_config_t config = memx_heap_config_default();
    memx_heap_t *heap = NULL;
    void *slots[32] = {0};
    size_t cursor = 0U;
    config.reserve_size = 4U * 1024U * 1024U;
    if (memx_heap_create(&config, &heap) != MEMX_HEAP_OK) {
        return 0;
    }
    while (cursor + 3U <= size) {
        const unsigned operation = data[cursor++] % 5U;
        const size_t slot = data[cursor++] % 32U;
        const size_t request = 1U + (size_t)data[cursor++] * 32U;
        switch (operation) {
            case 0U:
                if (slots[slot] == NULL) {
                    slots[slot] = memx_heap_malloc(heap, request);
                    if (slots[slot] != NULL) {
                        memset(slots[slot], (int)(request & 0xffU), request);
                    }
                }
                break;
            case 1U:
                if (slots[slot] != NULL) {
                    if (!memx_heap_free(heap, slots[slot])) {
                        abort();
                    }
                    slots[slot] = NULL;
                }
                break;
            case 2U:
                if (slots[slot] != NULL) {
                    void *replacement = memx_heap_realloc(
                        heap, slots[slot], request);
                    if (replacement != NULL) {
                        slots[slot] = replacement;
                    }
                }
                break;
            case 3U:
                if (slots[slot] == NULL) {
                    const size_t alignment = (size_t)1U
                        << (3U + (data[cursor - 1U] % 7U));
                    slots[slot] = memx_heap_aligned_alloc(
                        heap, alignment, request);
                }
                break;
            case 4U:
                if (slots[slot] == NULL) {
                    slots[slot] = memx_heap_calloc(heap, request, 1U);
                }
                break;
            default:
                abort();
        }
    }
    for (cursor = 0U; cursor < 32U; ++cursor) {
        if (slots[cursor] != NULL
            && !memx_heap_free(heap, slots[cursor])) {
            abort();
        }
    }
    memx_heap_thread_detach(heap);
    memx_heap_destroy(heap);
    return 0;
}
