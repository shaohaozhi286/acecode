#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"

TEST(ToolResultAttachments, FormatToolResultCarriesAttachmentContentParts) {
    acecode::ToolResult result;
    result.output = "created image";
    result.success = true;
    result.attachments = nlohmann::json::array({
        {
            {"id", "att_img"},
            {"session_id", "sid"},
            {"name", "plot.png"},
            {"kind", "image"},
            {"mime_type", "image/png"},
            {"path", "C:/tmp/plot.png"},
            {"blob_url", "/api/sessions/sid/attachments/att_img/blob"},
            {"size_bytes", 3},
        },
    });

    auto msg = acecode::ToolExecutor::format_tool_result("call-img", result);

    EXPECT_EQ(msg.role, "tool");
    EXPECT_EQ(msg.content, "created image");
    EXPECT_EQ(msg.tool_call_id, "call-img");
    ASSERT_TRUE(msg.content_parts.is_array());
    ASSERT_EQ(msg.content_parts.size(), 1u);
    EXPECT_EQ(msg.content_parts[0]["type"], "image");
    EXPECT_EQ(msg.content_parts[0]["attachment"]["id"], "att_img");
    EXPECT_EQ(msg.content_parts[0]["attachment"]["blob_url"],
              "/api/sessions/sid/attachments/att_img/blob");
}
