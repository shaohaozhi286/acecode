#pragma once

#include "../config/config.hpp"
#include "../provider/llm_provider.hpp"
#include "tool_executor.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace acecode {

struct VisionSubagentToolOptions {
    using ProviderFactory =
        std::function<std::shared_ptr<LlmProvider>(const ModelProfile&)>;
    using IndexChooser = std::function<std::size_t(std::size_t)>;

    ProviderFactory provider_factory;
    IndexChooser choose_index;
};

ToolImpl create_vision_analyze_tool(
    const AppConfig& config,
    VisionSubagentToolOptions options = {});

} // namespace acecode
