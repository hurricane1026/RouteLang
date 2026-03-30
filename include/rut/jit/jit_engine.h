#pragma once

#include "rut/common/types.h"

// Forward-declare LLVM C API opaque types to avoid pulling LLVM headers
// into runtime code. The actual LLVM headers are only included in
// jit_engine.cc and codegen.cc.
using LLVMOrcLLJITRef = struct LLVMOrcOpaqueLLJIT*;
using LLVMModuleRef = struct LLVMOpaqueModule*;
using LLVMContextRef = struct LLVMOpaqueContext*;

namespace rut::jit {

// ── JIT Engine ─────────────────────────────────────────────────────
// Wraps LLVM ORC LLJIT via the C API. Compiles LLVM IR modules to
// native code and resolves symbols (including runtime helpers).
//
// Thread safety:
//   - init() / shutdown() on main thread only
//   - compile() / lookup() are thread-safe (LLJIT uses internal locks)
//   - JIT'd function pointers are plain C calls, no LLVM dependency
//
// Lifetime:
//   - One JitEngine per process (not per shard)
//   - Shard threads never touch JitEngine — only call JIT'd fn ptrs

struct JitEngine {
    LLVMOrcLLJITRef lljit = nullptr;

    // Orphaned LLVMContextRefs from compile(). LLVM < 19 can't wrap an
    // existing context into a ThreadSafeContext, so the module's context
    // must be freed separately after LLJIT destroys the module.
    static constexpr u32 kMaxContexts = 64;
    LLVMContextRef contexts[kMaxContexts];
    u32 ctx_count = 0;

    bool init();

    // Compile an LLVM IR module to native code.
    // Always takes ownership of both mod and ctx regardless of return value.
    bool compile(LLVMModuleRef mod, LLVMContextRef ctx);

    void* lookup(const char* name);
    void shutdown();
};

}  // namespace rut::jit
