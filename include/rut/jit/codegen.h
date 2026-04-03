#pragma once

#include "rut/common/types.h"

// Forward declarations — avoid pulling LLVM headers into other code.
using LLVMModuleRef = struct LLVMOpaqueModule*;
using LLVMContextRef = struct LLVMOpaqueContext*;

namespace rut::rir {
struct Module;
}

namespace rut::jit {

// ── Codegen ────────────────────────────────────────────────────────
// Translates an RIR Module into an LLVM IR Module via the LLVM C API.
//
// Phase 1: handles sync handlers only (no yields). Each RIR Function
// becomes a single LLVM function with the HandlerFn signature.
//
// The returned LLVMModuleRef and LLVMContextRef are owned by the caller.
// Pass both to JitEngine::compile() which takes ownership.

struct CodegenResult {
    LLVMModuleRef mod;
    LLVMContextRef ctx;
    bool ok;
};

// Format the JIT handler symbol for an RIR function name.
// Returns the output length excluding the trailing '\0'.
u32 format_handler_symbol(Str name, char* out, u32 out_size);

// Translate all functions in the RIR module to LLVM IR.
// Returns {module, context, true} on success, {null, null, false} on error.
CodegenResult codegen(const rir::Module& rir_mod);

}  // namespace rut::jit
