#include "rut/sim/simulate_engine.h"

#include <unistd.h>

using namespace rut;

namespace {

static void write_str(i32 fd, const char* s) {
    u32 len = 0;
    while (s[len]) len++;
    (void)::write(fd, s, len);
}

static void usage() {
    write_str(2, "Usage: rut-simulate <manifest.txt> <capture.bin>\n");
    write_str(2, "Manifest format:\n");
    write_str(2, "  upstream <id> <name>\n");
    write_str(2, "  route <METHOD|ANY> <pattern> status <code>\n");
    write_str(2, "  route <METHOD|ANY> <pattern> proxy <upstream-id>\n");
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

    sim::ModuleContext module_ctx;
    if (!sim::build_module_from_manifest(manifest, module_ctx)) {
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
        const sim::SimulateResult result = sim::simulate_one(engine, entry);
        summary.total++;
        switch (result.verdict) {
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
        const u32 len = sim::format_result(result, line, sizeof(line));
        (void)::write(1, line, len);
    }

    char summary_buf[256];
    const u32 slen = sim::format_summary(summary, summary_buf, sizeof(summary_buf));
    (void)::write(1, summary_buf, slen);

    reader.close();
    engine.shutdown();
    module_ctx.destroy();
    return (summary.failed == 0 && summary.mismatched == 0 && summary.unsupported == 0) ? 0 : 1;
}
