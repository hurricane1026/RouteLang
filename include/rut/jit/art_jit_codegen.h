#pragma once

// ART JIT specialization — generates a per-trie specialized match()
// LLVM IR function for a populated ArtTrie. The generated code has:
//   - zero runtime type switch (each node's type/edge/terminals
//     emitted as IR constants at codegen time)
//   - per-method-slot terminal pickup (slot 0 = "any", plus G/P/D/H/
//     O/C/T as in route_trie.h's method_slot)
//   - first-match-wins via deepest-terminal-seen tracking
//
// The JIT'd function expects pre-canonicalized input (no leading '/',
// no trailing '/', no '?'/'#' bytes). Canon is lifted out into the
// HTTP parser's URI SIMD scan so it happens once per request rather
// than once per dispatch — see route_canon.h and http_parser.cc.
// Callers who hand a raw URI to ART must canonicalize first via
// canonicalize_request() (or use ArtTrie::match() / RouteConfig::match()
// which do that for you).
//
// Bench (PR #50 final): the JIT direct call runs at ~4 ns / match vs
// scalar ART's ~24 ns on a 32-route saas-shaped trie; the production
// hot path through RouteConfig::match_canonical lands at ~5 ns.
//
// Function signature of the generated code:
//   u16 match(const char* canon_ptr, u32 canon_len, u8 method)
//
// On success returns the JIT'd function pointer. On failure
// (codegen / verification / compile error) returns nullptr.

#include "rut/common/types.h"

namespace rut {
class ArtTrie;
}

namespace rut::jit {

struct JitEngine;

using ArtJitMatchFn = u16 (*)(const char* path_ptr, u32 path_len, u8 method);

// Generate a specialized match() for `trie` and JIT compile via
// `engine`. `unique_name` is the symbol the LLVM module exports
// (e.g., "art_match_<config_hash>"); caller chooses so collisions
// are caught.
ArtJitMatchFn art_jit_specialize(JitEngine& engine, const ArtTrie& trie, const char* unique_name);

}  // namespace rut::jit
