#include "rut/common/types.h"
#include "rut/runtime/traffic_capture.h"
#include "rut/runtime/traffic_replay.h"
#include "rut/sim/simulate_engine.h"

#include <errno.h>
#include <unistd.h>

using namespace rut;

namespace {

static bool write_all(i32 fd, const char* s, u32 len) {
    u32 pos = 0;
    while (pos < len) {
        const ssize_t n = ::write(fd, s + pos, len - pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        pos += static_cast<u32>(n);
    }
    return true;
}

static void write_str(i32 fd, const char* s) {
    u32 len = 0;
    while (s[len]) len++;
    (void)write_all(fd, s, len);
}

static void usage() {
    write_str(2, "Usage: rut-simulate <manifest.txt> <capture.bin>\n");
    write_str(2, "Manifest format:\n");
    write_str(2, "  upstream <id> <name>\n");
    write_str(2, "  route <METHOD|ANY> <pattern> status <code>\n");
    write_str(2, "  route <METHOD|ANY> <pattern> proxy <upstream-id>\n");
    write_str(2, "  pattern is prefix-matched and may include ':param' segments\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        usage();
        return 2;
    }

    sim::Manifest manifest;
    if (!sim::load_manifest(argv[1], manifest)) {
        write_str(2, "Failed to load manifest\n");
        return 1;
    }

    sim::ModuleContext module_ctx{};
    if (!sim::build_module_from_manifest(manifest, module_ctx)) {
        module_ctx.destroy();
        write_str(2, "Failed to build RIR module from manifest\n");
        return 1;
    }

    sim::Engine engine;
    if (!engine.init(module_ctx.module, manifest.upstreams, manifest.upstream_count)) {
        module_ctx.destroy();
        write_str(2, "Failed to initialize simulate engine\n");
        return 1;
    }

    ReplayReader reader;
    if (reader.open(argv[2]) != 0) {
        engine.shutdown();
        module_ctx.destroy();
        write_str(2, "Failed to open capture file\n");
        return 1;
    }

    CaptureEntry entry{};
    char line[512];
    sim::SimulateSummary summary{};
    while (reader.next(entry) == 0) {
        const sim::SimulateResult kResult = sim::simulate_one(engine, entry);
        summary.total++;
        switch (kResult.verdict) {
            case sim::Verdict::Match:
                summary.matched++;
                break;
            case sim::Verdict::Mismatch:
                summary.mismatched++;
                break;
            case sim::Verdict::Failed:
                summary.failed++;
                break;
            case sim::Verdict::Unsupported:
                summary.unsupported++;
                break;
        }
        const u32 kLen = sim::format_result(kResult, line, sizeof(line));
        (void)write_all(1, line, kLen);
    }

    char summary_buf[256];
    const u32 kSlen = sim::format_summary(summary, summary_buf, sizeof(summary_buf));
    (void)write_all(1, summary_buf, kSlen);

    reader.close();
    engine.shutdown();
    module_ctx.destroy();
    return (summary.failed == 0 && summary.mismatched == 0 && summary.unsupported == 0) ? 0 : 1;
}
