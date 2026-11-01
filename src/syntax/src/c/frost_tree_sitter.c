// SPDX-License-Identifier: Apache-2.0
//
// frost_tree_sitter.c: the vendored tree-sitter 0.27.1 runtime (third_party/tree-sitter, MIT) as
// one translation unit, with frost::syntax's own Unicode decoders (ADR 0023, D2).
/*
 * Upstream builds the runtime from lib/src/lib.c, which #includes every C file of lib/src. Its
 * lexer.c and query.c include ./unicode.h, which needs ICU headers we do not vendor, so this file
 * first defines that header's include guard and supplies the decoders itself
 * (frost_ts_unicode.h). The vendored bytes stay unmodified.
 *
 * Upstream code is not ours to fix, so its diagnostics are ignored between the push and the pop
 * below; this file and frost_ts_unicode.h compile under the full warning set as errors
 * (frost_apply_c_options OWN). WebAssembly support is off (TREE_SITTER_FEATURE_WASM undefined),
 * so wasm_store.c compiles to stubs.
 */
#define TREE_SITTER_UNICODE_H_
#include "frost_ts_unicode.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "lib.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
