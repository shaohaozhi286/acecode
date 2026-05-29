#include <gtest/gtest.h>

#include "config/config.hpp"
#include "session/attachment_store.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_executor.hpp"
#include "tool/vision_subagent_tool.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class CapturingProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>& tools) override {
        messages_ = messages;
        tools_ = tools;
        acecode::ChatResponse response;
        response.content = "vision result";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&,
        const acecode::StreamCallback& callback,
        std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    std::string name() const override { return "capturing"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "capturing-model"; }
    void set_model(const std::string&) override {}

    std::vector<acecode::ChatMessage> messages_;
    std::vector<acecode::ToolDef> tools_;
};

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_vision_tool_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::ModelProfile model_profile(
    const std::string& name,
    std::vector<std::string> capabilities) {
    acecode::ModelProfile profile;
    profile.name = name;
    profile.provider = "openai";
    profile.base_url = "http://localhost/v1";
    profile.api_key = "sk-test";
    profile.model = name + "-id";
    profile.capabilities = std::move(capabilities);
    return profile;
}

acecode::ChatMessage user_with_image(const acecode::AttachmentRecord& record) {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "see image";
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "see image"}},
        nlohmann::json{{"type", "image"}, {"attachment", acecode::attachment_to_json(record)}},
    });
    return msg;
}

acecode::AttachmentRecord save_image(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& name) {
    std::string error;
    auto record = acecode::save_attachment(
        project_dir,
        session_id,
        name,
        "image/png",
        "png-bytes-" + name,
        &error);
    if (!record.has_value()) {
        ADD_FAILURE() << error;
        return {};
    }
    return *record;
}

} // namespace

TEST(VisionSubagentTool, MissingVisionModelFailsClearly) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("text-only", {"tool_use"}));
    auto tool = acecode::create_vision_analyze_tool(cfg);

    acecode::ToolContext ctx;
    auto result = tool.execute(
        R"({"prompt":"what is in the image?","attachment":{"id":"att_1","session_id":"s","name":"screen.png","kind":"image","mime_type":"image/png","path":"x","blob_url":"/b","size_bytes":1}})",
        ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NO_VISION_MODEL");
}

TEST(VisionSubagentTool, UsesLatestActiveSessionImageAndDoesNotPersistChildCall) {
    auto cwd = temp_cwd("latest");
    const std::string session_id = "sid-vision-latest";
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(cwd));
    fs::remove_all(project_dir);

    acecode::SessionManager sm;
    sm.start_session(acecode::path_to_utf8(cwd), "stub", "stub-model", session_id);
    auto first = save_image(project_dir, session_id, "first.png");
    auto second = save_image(project_dir, session_id, "second.png");
    sm.on_message(user_with_image(first));
    sm.on_message(user_with_image(second));
    const auto before = sm.load_active_messages().size();
    const auto sessions_before = acecode::SessionStorage::list_sessions(project_dir).size();

    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-one", {"vision", "tool_use"}));
    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    acecode::ToolContext ctx;
    ctx.session_manager = &sm;
    auto result = tool.execute(R"({"prompt":"describe the screenshot"})", ctx);

    EXPECT_TRUE(result.success) << result.output;
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["model_name"], "vision-one");
    EXPECT_EQ(json["attachment_id"], second.id);
    EXPECT_EQ(selected.name, "vision-one");
    ASSERT_EQ(provider->messages_.size(), 1u);
    ASSERT_TRUE(provider->tools_.empty());
    ASSERT_TRUE(provider->messages_[0].content_parts.is_array());
    EXPECT_EQ(provider->messages_[0].content_parts[1]["attachment"]["id"], second.id);
    EXPECT_EQ(sm.load_active_messages().size(), before)
        << "internal vision call must not append child messages to the active session";
    EXPECT_EQ(acecode::SessionStorage::list_sessions(project_dir).size(), sessions_before)
        << "internal vision call must not create a resumable child session";

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(VisionSubagentTool, ExplicitModelAndAttachmentAreHonored) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-a", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-b", {"vision"}));

    acecode::AttachmentRecord record;
    record.id = "att_explicit";
    record.session_id = "sid";
    record.name = "screen.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = "C:/tmp/screen.png";
    record.blob_url = "/api/sessions/sid/attachments/att_explicit/blob";
    record.size_bytes = 3;

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    opts.choose_index = [](std::size_t) { return 0u; };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    nlohmann::json args = {
        {"prompt", "read it"},
        {"model_name", "vision-b"},
        {"attachment", acecode::attachment_to_json(record)},
    };
    acecode::ToolContext ctx;
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-b");
    ASSERT_EQ(provider->messages_.size(), 1u);
    EXPECT_EQ(provider->messages_[0].content_parts[1]["attachment"]["id"], "att_explicit");
}

TEST(VisionSubagentTool, RandomChooserIsUsedWhenMultipleVisionModelsExist) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-a", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-b", {"vision"}));

    acecode::AttachmentRecord record;
    record.id = "att";
    record.session_id = "sid";
    record.name = "screen.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = "C:/tmp/screen.png";
    record.blob_url = "/api/sessions/sid/attachments/att/blob";
    record.size_bytes = 1;

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    opts.choose_index = [](std::size_t count) {
        EXPECT_EQ(count, 2u);
        return 1u;
    };

    auto tool = acecode::create_vision_analyze_tool(cfg, opts);
    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(record)},
    };
    acecode::ToolContext ctx;
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-b");
}

TEST(VisionSubagentTool, RegisteredInSharedBuiltinToolSet) {
    acecode::AppConfig cfg;
    cfg.web_search.enabled = false;
    cfg.ace_browser_bridge.enabled = false;
    acecode::ToolExecutor tools;

    acecode::register_session_builtin_tools(tools, cfg);

    EXPECT_TRUE(tools.has_tool("vision_analyze"));
    EXPECT_TRUE(tools.is_read_only("vision_analyze"));
}

namespace {

// 把若干字节写到 cwd 下的文件,返回绝对路径。模拟 browser/shell 工具写出的截图。
std::string write_file(const fs::path& dir, const std::string& name,
                       const std::string& bytes) {
    const fs::path p = dir / name;
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ofs.close();
    return acecode::path_to_utf8(p);
}

struct VisionPathFixture {
    fs::path cwd;
    std::string project_dir;
    acecode::SessionManager sm;
    acecode::AppConfig cfg;
    std::shared_ptr<CapturingProvider> provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;

    explicit VisionPathFixture(const std::string& hint) {
        cwd = temp_cwd(hint);
        project_dir = acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(cwd));
        fs::remove_all(project_dir);
        sm.start_session(acecode::path_to_utf8(cwd), "stub", "stub-model",
                         "sid-vision-" + hint);
        cfg.saved_models.push_back(model_profile("vision-one", {"vision"}));
    }

    ~VisionPathFixture() {
        fs::remove_all(project_dir);
        fs::remove_all(cwd);
    }

    acecode::ToolImpl tool() {
        acecode::VisionSubagentToolOptions opts;
        opts.provider_factory = [this](const acecode::ModelProfile& profile) {
            selected = profile;
            return provider;
        };
        return acecode::create_vision_analyze_tool(cfg, opts);
    }
};

} // namespace

// 场景:image_path 用相对路径(基于 ToolContext.cwd)→ 读取 + 物化 + 发送给视觉模型。
TEST(VisionSubagentTool, ImagePathRelativeIsMaterializedAndSent) {
    VisionPathFixture fx("path_rel");
    write_file(fx.cwd, "shot.png", "fake-png-bytes");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"shot.png"})", ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(fx.selected.name, "vision-one");
    ASSERT_EQ(fx.provider->messages_.size(), 1u);
    ASSERT_TRUE(fx.provider->messages_[0].content_parts.is_array());
    // 物化后的附件应作为 image part 发给视觉模型。
    EXPECT_EQ(fx.provider->messages_[0].content_parts[1]["type"], "image");
}

// 场景:image_path 用 workspace 外的绝对路径 → 同样物化并发送。
TEST(VisionSubagentTool, ImagePathAbsoluteOutsideWorkspace) {
    VisionPathFixture fx("path_abs");
    // 写到一个独立的临时目录(不在 fx.cwd 之下),用绝对路径引用。
    auto outside = temp_cwd("path_abs_outside");
    const std::string abs_path = write_file(outside, "external.png", "fake-png-bytes");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    nlohmann::json args = {{"prompt", "describe"}, {"image_path", abs_path}};
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    ASSERT_EQ(fx.provider->messages_.size(), 1u);

    fs::remove_all(outside);
}

// 场景:image_path 指向非图片文件 → 清晰拒绝。
TEST(VisionSubagentTool, ImagePathNonImageRejected) {
    VisionPathFixture fx("path_notimg");
    write_file(fx.cwd, "notes.txt", "plain text");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"notes.txt"})", ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NOT_IMAGE");
}

// 场景:image_path 指向不存在的路径 → 清晰拒绝。
TEST(VisionSubagentTool, ImagePathMissingRejected) {
    VisionPathFixture fx("path_missing");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"nope.png"})", ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "IMAGE_PATH_NOT_FOUND");
}

// 场景:超大 image_path 在读取字节前就被大小校验拒绝(sparse file,瞬时创建)。
TEST(VisionSubagentTool, ImagePathTooLargeRejectedBeforeRead) {
    VisionPathFixture fx("path_toolarge");
    const fs::path big = fx.cwd / "big.png";
    {
        std::ofstream ofs(big, std::ios::binary);
        ofs.put('x');
        ofs.close();
    }
    std::error_code ec;
    fs::resize_file(big, acecode::kMaxAttachmentBytes + 1, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"big.png"})", ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "IMAGE_TOO_LARGE");
}

// 场景:无 active session 时使用 image_path → 清晰拒绝,不崩溃。
TEST(VisionSubagentTool, ImagePathWithoutActiveSessionRejected) {
    auto cwd = temp_cwd("path_nosession");
    const std::string abs_path = write_file(cwd, "shot.png", "fake-png-bytes");

    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-one", {"vision"}));
    auto provider = std::make_shared<CapturingProvider>();
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile&) { return provider; };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    acecode::ToolContext ctx;  // 无 session_manager
    ctx.cwd = acecode::path_to_utf8(cwd);
    nlohmann::json args = {{"prompt", "describe"}, {"image_path", abs_path}};
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NO_ACTIVE_SESSION");

    fs::remove_all(cwd);
}

// 场景:无任何视觉模型时使用 image_path → 在物化前就因 NO_VISION_MODEL 拒绝。
TEST(VisionSubagentTool, ImagePathRejectedWhenNoVisionModel) {
    auto cwd = temp_cwd("path_novision");
    const std::string abs_path = write_file(cwd, "shot.png", "fake-png-bytes");

    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("text-only", {"tool_use"}));
    auto tool = acecode::create_vision_analyze_tool(cfg);

    acecode::SessionManager sm;
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(cwd));
    fs::remove_all(project_dir);
    sm.start_session(acecode::path_to_utf8(cwd), "stub", "stub-model", "sid-novision");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(cwd);
    ctx.session_manager = &sm;
    nlohmann::json args = {{"prompt", "describe"}, {"image_path", abs_path}};
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NO_VISION_MODEL");

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}
