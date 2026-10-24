#pragma once
#include <cstdlib>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
inline void *heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void *heap_caps_calloc(size_t n, size_t size, unsigned) { return std::calloc(n, size); }
inline void heap_caps_free(void *p) { std::free(p); }
inline size_t heap_caps_get_free_size(unsigned) { return 100000; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return 50000; }
