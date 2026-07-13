#include "headless/headless_jsonl.hpp"

#include <csignal>
#include <cstdio>
#include <string>

using acecode::headless::JsonlStreamWriter;

namespace {

nlohmann::json record(const std::string& type, int index = 0,
                      std::string payload = {}) {
    nlohmann::json value{
        {"type", type},
        {"timestamp", 1000 + index},
        {"sessionID", "process-test"},
        {"index", index},
    };
    if (!payload.empty()) value["payload"] = std::move(payload);
    return value;
}

int run_staged(JsonlStreamWriter& writer) {
    if (!writer.write_record(record("step_start"))) return 23;
    if (std::getchar() == EOF) return 24;
    if (!writer.write_record(record("text", 1, "ready"))) return 23;
    if (!writer.write_record(record("step_finish", 2))) return 23;
    return 0;
}

int run_bulk(JsonlStreamWriter& writer) {
    const std::string payload(16 * 1024, 'x');
    for (int i = 0; i < 256; ++i) {
        if (!writer.write_record(record("text", i, payload))) return 23;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    JsonlStreamWriter writer(stdout);
    const std::string mode = argc > 1 ? argv[1] : "staged";
    if (mode == "staged") return run_staged(writer);
    if (mode == "bulk") return run_bulk(writer);
    return 64;
}
