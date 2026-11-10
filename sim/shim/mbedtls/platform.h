#pragma once
// TLS is WinHTTP's job in the sim; the firmware's PSRAM allocator hook is a no-op.
#include <stddef.h>
static inline int mbedtls_platform_set_calloc_free(void* (*)(size_t, size_t), void (*)(void*)) { return 0; }
