#include "runtime.hpp"

#include "backend_router.hpp"
#include "region_detector.hpp"
#include "utils/logger.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace acecode::web_search {

struct Runtime::Impl {
    explicit Impl(const WebSearchConfig& c)
        : cfg_(c), router_(cfg_), detector_(c.timeout_ms > 0 ? c.timeout_ms : 2000) {}
    WebSearchConfig cfg_;
    BackendRouter router_;
    RegionDetector detector_;
};

Runtime::Runtime(const WebSearchConfig& cfg)
    : impl_(new Impl(cfg)) {}

Runtime::~Runtime() {
    delete impl_;
}

BackendRouter& Runtime::router() { return impl_->router_; }
RegionDetector& Runtime::detector() { return impl_->detector_; }
const WebSearchConfig& Runtime::cfg() const { return impl_->cfg_; }

namespace {
std::mutex g_mu;
std::unique_ptr<Runtime> g_runtime;
} // namespace

void init(const WebSearchConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_runtime) {
        LOG_WARN("[web_search] init called twice; ignoring second call");
        return;
    }
    g_runtime = std::make_unique<Runtime>(cfg);
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_runtime.reset();
}

bool is_initialized() {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<bool>(g_runtime);
}

Runtime& runtime() {
    std::lock_guard<std::mutex> lk(g_mu);
    return *g_runtime;
}

} // namespace acecode::web_search
