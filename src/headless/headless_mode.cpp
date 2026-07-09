#include "headless_mode.hpp"

#include <atomic>

namespace acecode::headless {

namespace {
std::atomic<bool> g_headless_active{false};
}

void set_active(bool on) { g_headless_active.store(on, std::memory_order_relaxed); }

bool active() { return g_headless_active.load(std::memory_order_relaxed); }

} // namespace acecode::headless
