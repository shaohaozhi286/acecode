#include "tcp_probe.hpp"

#include <mutex>

namespace acecode::network {

namespace {

std::mutex& probe_mu() {
    static std::mutex m;
    return m;
}

TcpProbeFn& probe_override() {
    static TcpProbeFn fn;
    return fn;
}

} // namespace

const char* tcp_probe_reason_name(TcpProbeReason r) {
    switch (r) {
        case TcpProbeReason::Ok:      return "Ok";
        case TcpProbeReason::Refused: return "Refused";
        case TcpProbeReason::Timeout: return "Timeout";
        case TcpProbeReason::DnsFail: return "DnsFail";
        case TcpProbeReason::Other:   return "Other";
    }
    return "Other";
}

void set_tcp_probe_for_testing(TcpProbeFn fn) {
    std::lock_guard<std::mutex> lk(probe_mu());
    probe_override() = std::move(fn);
}

TcpProbeFn current_tcp_probe() {
    std::lock_guard<std::mutex> lk(probe_mu());
    if (probe_override()) return probe_override();
    return [](const std::string& h, int p, int t) { return tcp_probe(h, p, t); };
}

} // namespace acecode::network
