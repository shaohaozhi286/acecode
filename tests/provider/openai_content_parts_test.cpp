// tests/provider/openai_content_parts_test.cpp
// 覆盖 route-attachments-by-capability 的 provider 序列化 gate(tasks 1.4)。
// openai_content_for_message 是 image/file 附件路由的唯一收口点,这里直接调用它,
// 断言:
//   - 视觉模型能拿到真实 image_url part;
//   - 非视觉模型拿到句柄文本而非图片,且 fallback 措辞随是否存在视觉模型变化;
//   - 非图片附件(text-like / PDF / SVG / 误标成 image 的非图片)永远不产生 image_url。
#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"
#include "session/attachment_store.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_project(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_content_parts_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::AttachmentRecord save(const std::string& project_dir,
                               const std::string& name,
                               const std::string& mime,
                               const std::string& bytes) {
    std::string error;
    auto record = acecode::save_attachment(
        project_dir, "sid-content", name, mime, bytes, &error);
    EXPECT_TRUE(record.has_value()) << error;
    return record.value_or(acecode::AttachmentRecord{});
}

acecode::ChatMessage image_part_message(const acecode::AttachmentRecord& record) {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "look";
    // 故意用 type=image,模拟 UI/历史可能产生的 image part;gate/分类应自行判定。
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "look"}},
        nlohmann::json{{"type", "image"}, {"attachment", acecode::attachment_to_json(record)}},
    });
    return msg;
}

bool has_image_url(const nlohmann::json& content) {
    if (!content.is_array()) return false;
    for (const auto& part : content) {
        if (part.is_object() && part.value("type", std::string{}) == "image_url") {
            return true;
        }
    }
    return false;
}

std::string collect_text(const nlohmann::json& content) {
    std::string text;
    if (!content.is_array()) {
        return content.is_string() ? content.get<std::string>() : std::string{};
    }
    for (const auto& part : content) {
        if (part.is_object() && part.value("type", std::string{}) == "text") {
            text += part.value("text", std::string{});
            text += "\n";
        }
    }
    return text;
}

} // namespace

// 场景:视觉模型 + 图片附件 → 真实 image_url part。
TEST(OpenAiContentParts, VisionModelGetsImageUrl) {
    auto dir = temp_project("vision_ok");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true,
        /*any_vision_model_available=*/false);

    EXPECT_TRUE(has_image_url(content)) << content.dump(2);
    fs::remove_all(dir);
}

// 场景:非视觉模型 + 存在其它视觉模型 → 无图片 payload,fallback 引导用 vision_analyze。
TEST(OpenAiContentParts, NonVisionModelGetsHandlePointingToVisionAnalyze) {
    auto dir = temp_project("no_vision_but_available");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/false,
        /*any_vision_model_available=*/true);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    const std::string text = collect_text(content);
    EXPECT_NE(text.find("vision_analyze"), std::string::npos) << text;
    fs::remove_all(dir);
}

// 场景:非视觉模型 + 系统无任何视觉模型 → 无图片 payload,fallback 说明需配置视觉模型。
TEST(OpenAiContentParts, NonVisionModelNoVisionAvailableExplainsConfig) {
    auto dir = temp_project("no_vision_at_all");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/false,
        /*any_vision_model_available=*/false);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    const std::string text = collect_text(content);
    EXPECT_NE(text.find("vision"), std::string::npos) << text;
    // 不应误导用户去调用 vision_analyze —— 因为系统根本没有可用视觉模型。
    EXPECT_EQ(text.find("vision_analyze tool"), std::string::npos) << text;
    fs::remove_all(dir);
}

// 场景:text-like 文件(file part)永远不会变成图片。
TEST(OpenAiContentParts, TextFileNeverImage) {
    auto dir = temp_project("textfile");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "notes.txt", "text/plain", "hello world");

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "file"}, {"attachment", acecode::attachment_to_json(record)}},
    });

    auto content = acecode::openai_content_for_message(msg, true, true);
    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    EXPECT_NE(collect_text(content).find("notes.txt"), std::string::npos);
    fs::remove_all(dir);
}

// 场景:PDF 即便被标成 image part、即便模型支持 vision,也不会变成图片 payload。
TEST(OpenAiContentParts, PdfStaysFileEvenWithVision) {
    auto dir = temp_project("pdf");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "doc.pdf", "application/pdf", "%PDF-1.4 fake");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true, true);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    fs::remove_all(dir);
}

// 场景:SVG 虽然 MIME 以 image/ 开头,但被排除出 vision 路由,不产生 image_url。
TEST(OpenAiContentParts, SvgNotSerializedAsImage) {
    auto dir = temp_project("svg");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "vec.svg", "image/svg+xml", "<svg></svg>");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true, true);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    fs::remove_all(dir);
}

// 场景:误标 —— attachment 元数据是非图片(application/pdf)却被包成 image part,
// 序列化层必须据 MIME 把它降级为文件句柄,绝不发图片 payload。
TEST(OpenAiContentParts, MislabeledNonImageDowngradedToFile) {
    acecode::ChatMessage msg;
    msg.role = "user";
    // 手工构造误标:kind=image 但 mime 是 application/pdf。
    nlohmann::json attachment = {
        {"id", "att_x"}, {"session_id", "s"}, {"name", "report.pdf"},
        {"kind", "image"}, {"mime_type", "application/pdf"},
        {"path", "x"}, {"blob_url", "/b"}, {"size_bytes", 10},
    };
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "image"}, {"attachment", attachment}},
    });

    auto content = acecode::openai_content_for_message(msg, /*model_has_vision=*/true, true);
    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
}

// 分类源头:attachment_kind_for_mime 把 SVG 归为 file,其它图片仍为 image。
TEST(OpenAiContentParts, AttachmentKindClassification) {
    EXPECT_EQ(acecode::attachment_kind_for_mime("image/png", "a.png"), "image");
    EXPECT_EQ(acecode::attachment_kind_for_mime("image/svg+xml", "a.svg"), "file");
    EXPECT_EQ(acecode::attachment_kind_for_mime("application/pdf", "a.pdf"), "file");
    EXPECT_EQ(acecode::attachment_kind_for_mime("text/plain", "a.txt"), "file");
}
