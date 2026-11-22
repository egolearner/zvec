// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snowball_workspace.h"

// Only this private Snowball runtime translation unit uses the wrapper.
// Keep the host compiler, generated languages and all other libraries on
// their ordinary CRT. The Snowball target uses C99 on all platforms.
#if defined(_MSC_VER)
static __declspec(thread) size_t *remaining_bytes;
#else
static __thread size_t *remaining_bytes;
#endif

size_t *zvec_fts_set_snowball_budget(size_t *remaining) {
  size_t *previous = remaining_bytes;
  remaining_bytes = remaining;
  return previous;
}

static void *bounded_realloc(void *old, size_t bytes) {
  if (remaining_bytes) {
    // Charge the whole new buffer, even when realloc can grow in place.
    // This covers the peak when the CRT must keep both allocations alive.
    const size_t overhead = 64;
    if (*remaining_bytes < overhead || bytes > *remaining_bytes - overhead) {
      return NULL;
    }
    *remaining_bytes -= bytes + overhead;
  }
  return realloc(old, bytes);
}

// utilities.c owns the single dynamic string growth operation used by all
// generated Snowball 3.1.1 UTF-8 programs, including auxiliary strings.
#define realloc(ptr, bytes) bounded_realloc(ptr, bytes)
#include "utilities.c"
#undef realloc
