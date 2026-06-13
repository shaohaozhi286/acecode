#include "show_image_tool.hpp"

#include "tool_icons.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace acecode {

namespace {

std::string mime_for_extension(const std::string& ext_lower) {
    if (ext_lower == ".png")  return "image/png";
    if (ext_lower == ".jpg" || ext_lower == ".jpeg") return "image/jpeg";
    if (ext_lower == ".gif")  return "image/gif";
    if (ext_lower == ".webp") return "image/webp";
    if (ext_lower == ".bmp")  return "image/bmp";
    return {};
}

} // namespace

static ToolResult execute_show_image(const std::string& arguments_json,
                                     const ToolContext& ctx) {
    ToolArgsParser parser(arguments_json);
    if (parser.has_error()) return ToolResult{parser.error(), false};

    std::string image_path = parser.get_or<std::string>("image_path", "");
    std::string title      = parser.get_or<std::string>("title", "");

    if (image_path.empty())
        return ToolResult{ToolErrors::missing_parameter("image_path"), false};

    namespace fs = std::filesystem;
    fs::path p(image_path);
    if (p.is_relative() && !ctx.cwd.empty())
        p = fs::path(ctx.cwd) / p;

    std::error_code ec;
    if (!fs::exists(p, ec) || ec)
        return ToolResult{"Image file not found: " + p.string(), false};
    if (!fs::is_regular_file(p, ec) || ec)
        return ToolResult{"Not a regular file: " + p.string(), false};

    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    const std::string mime = mime_for_extension(ext);
    if (mime.empty())
        return ToolResult{
            "Unsupported image format: " + ext +
            " (supported: .png .jpg .jpeg .gif .webp .bmp)", false};

    const std::string name     = p.filename().string();
    const std::string abs_path = p.lexically_normal().string();
    if (title.empty()) title = name;

    const auto file_size = fs::file_size(p, ec);

    nlohmann::json attachments = nlohmann::json::array();
    attachments.push_back({
        {"name",      name},
        {"mime_type", mime},
        {"path",      abs_path},
    });

    ToolSummary summary;
    summary.verb   = "Showed";
    summary.object = title;
    summary.icon   = tool_icon("show_image");
    if (!ec && file_size != static_cast<std::uintmax_t>(-1))
        summary.metrics.emplace_back("size", format_bytes_compact(file_size));

    ToolResult result{"Image: " + abs_path, true};
    result.summary     = std::move(summary);
    result.attachments = std::move(attachments);
    return result;
}

ToolImpl create_show_image_tool() {
    ToolDef def;
    def.name        = "show_image";
    def.description =
        "Display an image file to the user. Call this when you have "
        "produced, downloaded, or captured an image and want to show it. "
        "Supported formats: PNG, JPEG, GIF, WebP, BMP.";
    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"image_path"})},
        {"properties", {
            {"image_path", {
                {"type",        "string"},
                {"description", "Absolute or cwd-relative path to the image file."},
            }},
            {"title", {
                {"type",        "string"},
                {"description", "Optional display title shown in the chat (defaults to filename)."},
            }},
        }},
    };
    ToolImpl impl;
    impl.definition   = def;
    impl.execute      = execute_show_image;
    impl.is_read_only = true;
    impl.source       = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
