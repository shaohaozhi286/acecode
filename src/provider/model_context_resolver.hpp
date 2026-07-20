#pragma once

#include "../config/config.hpp"

#include <string>

namespace acecode {

int resolve_model_context_window(
    const AppConfig& config,
    const std::string& provider_name,
    const std::string& model,
    int fallback_context_window
);

// Session-facing variant: never waits for remote `/models` endpoint metadata.
// It returns cached/local models.dev context when available, otherwise returns
// fallback_context_window immediately and warms endpoint metadata in the
// background for future calls.
int resolve_model_context_window_nonblocking(
    const AppConfig& config,
    const std::string& provider_name,
    const std::string& model,
    int fallback_context_window
);

int resolve_model_profile_context_window(
    const AppConfig& config,
    const ModelProfile& profile,
    int fallback_context_window
);

int resolve_model_profile_context_window_nonblocking(
    const AppConfig& config,
    const ModelProfile& profile,
    int fallback_context_window
);

// Runtime session state also accepts a discovered model-pool window. A
// positive per-profile override remains authoritative over that discovered
// value; otherwise the pool value keeps its existing priority over
// local/cache/fallback resolution.
int resolve_runtime_model_profile_context_window_nonblocking(
    const AppConfig& config,
    const ModelProfile& profile,
    int fallback_context_window,
    int model_pool_context_window
);

// Test helper for process-local cache/in-flight state.
void reset_model_context_window_cache_for_test();

} // namespace acecode
