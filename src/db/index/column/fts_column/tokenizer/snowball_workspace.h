// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// NULL selects ordinary libstemmer allocation. The caller owns the counter
// and restores the previous scope before the counter goes out of scope.
size_t *zvec_fts_set_snowball_budget(size_t *remaining);

#ifdef __cplusplus
}
#endif
