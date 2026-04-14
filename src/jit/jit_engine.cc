#include "rut/jit/jit_engine.h"

#include "rut/jit/runtime_helpers.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <unistd.h>  // write (for error logging)

namespace rut::jit {

// ── Error logging (no stdlib) ──────────────────────────────────────

static void log_error(const char* prefix, LLVMErrorRef err) {
    char* msg = LLVMGetErrorMessage(err);
    auto write_str = [](const char* s) {
        int len = 0;
        while (s[len]) len++;
        (void)::write(2, s, len);
    };
    write_str(prefix);
    write_str(": ");
    write_str(msg);
    write_str("\n");
    LLVMDisposeErrorMessage(msg);
}

// ── Runtime Helper Symbol Table ────────────────────────────────────

struct HelperEntry {
    const char* name;
    void* addr;
};

// Null-terminated table of runtime helper symbols.
static const HelperEntry kHelpers[] = {
    {"rut_helper_req_path", reinterpret_cast<void*>(&rut_helper_req_path)},
    {"rut_helper_req_method", reinterpret_cast<void*>(&rut_helper_req_method)},
    {"rut_helper_req_header", reinterpret_cast<void*>(&rut_helper_req_header)},
    {"rut_helper_req_remote_addr", reinterpret_cast<void*>(&rut_helper_req_remote_addr)},
    {"rut_helper_str_has_prefix", reinterpret_cast<void*>(&rut_helper_str_has_prefix)},
    {"rut_helper_str_eq", reinterpret_cast<void*>(&rut_helper_str_eq)},
    {"rut_helper_str_cmp", reinterpret_cast<void*>(&rut_helper_str_cmp)},
    {"rut_helper_str_trim_prefix", reinterpret_cast<void*>(&rut_helper_str_trim_prefix)},
    {nullptr, nullptr},
};

// ── JitEngine ──────────────────────────────────────────────────────

bool JitEngine::init() {
    // Initialize native target (required before creating LLJIT).
    if (LLVMInitializeNativeTarget()) return false;
    LLVMInitializeNativeAsmPrinter();
    ctx_count = 0;

    // Create LLJIT with default settings (auto-detects host target).
    LLVMErrorRef err = LLVMOrcCreateLLJIT(&lljit, nullptr);
    if (err) {
        log_error("jit: LLJIT creation failed", err);
        return false;
    }

    // Pre-register all runtime helper symbols as absolute addresses.
    // MangleAndIntern produces correctly mangled names for the target.
    LLVMOrcJITDylibRef main_jd = LLVMOrcLLJITGetMainJITDylib(lljit);

    u32 count = 0;
    for (const auto* h = kHelpers; h->name; h++) count++;

    LLVMOrcCSymbolMapPair pairs[16];
    for (u32 i = 0; i < count && i < 16; i++) {
        pairs[i].Name = LLVMOrcLLJITMangleAndIntern(lljit, kHelpers[i].name);
        pairs[i].Sym.Address = reinterpret_cast<LLVMOrcExecutorAddress>(kHelpers[i].addr);
        pairs[i].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
        pairs[i].Sym.Flags.TargetFlags = 0;
    }

    // LLVMOrcAbsoluteSymbols takes ownership of the Name fields in pairs.
    // Do NOT release them after this call.
    LLVMOrcMaterializationUnitRef mu = LLVMOrcAbsoluteSymbols(pairs, count);
    err = LLVMOrcJITDylibDefine(main_jd, mu);
    if (err) {
        log_error("jit: helper registration failed", err);
        // On failure, ownership of mu stays with us.
        LLVMOrcDisposeMaterializationUnit(mu);
        LLVMOrcDisposeLLJIT(lljit);
        lljit = nullptr;
        return false;
    }

    return true;
}

bool JitEngine::compile(LLVMModuleRef mod, LLVMContextRef ctx) {
    // Always takes ownership of mod and ctx regardless of return value.
    if (!lljit) {
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return false;
    }

    // Set the module's data layout and target triple from LLJIT.
    const char* dl = LLVMOrcLLJITGetDataLayoutStr(lljit);
    if (dl) LLVMSetDataLayout(mod, dl);
    const char* triple = LLVMOrcLLJITGetTripleString(lljit);
    if (triple) LLVMSetTarget(mod, triple);

    char* verify_msg = nullptr;
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_msg)) {
        if (verify_msg) {
            auto write_str = [](const char* s) {
                int len = 0;
                while (s[len]) len++;
                (void)::write(2, s, len);
            };
            write_str("jit: module verification failed: ");
            write_str(verify_msg);
            write_str("\n");
            LLVMDisposeMessage(verify_msg);
        }
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return false;
    }

    // Wrap module into a ThreadSafeModule for submission to LLJIT.
    // We create a fresh ThreadSafeContext as the lock wrapper. Its internal
    // context differs from the module's context, but LLJIT only uses it
    // for synchronization — the module's own context is used for compilation.
    // The module's original LLVMContext (ctx) is tracked for cleanup in
    // shutdown() after LLJIT has destroyed the module.
    LLVMOrcThreadSafeContextRef tsctx = LLVMOrcCreateNewThreadSafeContext();
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(mod, tsctx);
    LLVMOrcDisposeThreadSafeContext(tsctx);

    // Track the orphaned context for cleanup.
    if (ctx_count < kMaxContexts) {
        contexts[ctx_count++] = ctx;
    }

    // Submit to the main JITDylib. On success, LLJIT owns tsm.
    LLVMOrcJITDylibRef main_jd = LLVMOrcLLJITGetMainJITDylib(lljit);
    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(lljit, main_jd, tsm);
    if (err) {
        log_error("jit: module compilation failed", err);
        // On failure, ownership of tsm stays with us.
        LLVMOrcDisposeThreadSafeModule(tsm);
        return false;
    }

    return true;
}

void* JitEngine::lookup(const char* name) {
    if (!lljit) return nullptr;

    LLVMOrcExecutorAddress addr = 0;
    LLVMErrorRef err = LLVMOrcLLJITLookup(lljit, &addr, name);
    if (err) {
        log_error("jit: symbol lookup failed", err);
        return nullptr;
    }

    // Cast via uintptr_t. The NOLINT suppresses performance-no-int-to-ptr
    // which is unavoidable here — the LLVM C API returns addresses as u64.
    return reinterpret_cast<void*>(  // NOLINT(performance-no-int-to-ptr)
        static_cast<uintptr_t>(addr));
}

void JitEngine::shutdown() {
    // Dispose LLJIT first — this destroys all compiled modules.
    if (lljit) {
        LLVMErrorRef err = LLVMOrcDisposeLLJIT(lljit);
        if (err) {
            log_error("jit: LLJIT disposal failed", err);
        }
        lljit = nullptr;
    }
    // Now safe to dispose orphaned contexts (modules that referenced
    // them have been destroyed by LLJIT above).
    for (u32 i = 0; i < ctx_count; i++) {
        LLVMContextDispose(contexts[i]);
    }
    ctx_count = 0;
}

}  // namespace rut::jit
