// 覆盖 src/tool/ask_user_question_tool.cpp 的纯函数路径:
//   1. validate_ask_user_question_args 的合法输入 / 非法输入分支(问题数 /
//      选项数 / header 长度 / 问题文本唯一性 / 选项 label 唯一性 / preview
//      字段容忍)
//   2. format_ask_answers 的拼接契约(单题、多题 + multi-select、引号不转义)
//   3. make_rejected_ask_result 的固定拒绝文本
// TUI overlay 的事件 / 渲染 / 阻塞协议归手动集成测试(在 CLAUDE.md
// "Unit tests" 一节说明的 TUI exemption 范围内),这里不覆盖。

#include <gtest/gtest.h>

#include "tool/ask_user_question_tool.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using acecode::AskQuestion;
using acecode::AskOption;
using acecode::format_ask_answers;
using acecode::make_rejected_ask_result;
using acecode::validate_ask_user_question_args;

// 场景:合法最小输入(1 题 2 选项,均含必填字段)应通过校验,并把
// question / header / options / multiSelect 回填到结构里。
TEST(AskUserQuestionValidateTest, MinimalValidInputIsAccepted) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({
            "questions": [{
                "question": "Which library?",
                "header": "Library",
                "options": [
                    {"label": "axios", "description": "HTTP with promises"},
                    {"label": "fetch", "description": "Native browser API"}
                ]
            }]
        })", err);
    ASSERT_TRUE(out.has_value()) << err;
    EXPECT_TRUE(err.empty());
    ASSERT_EQ(out->size(), 1u);
    EXPECT_EQ((*out)[0].question, "Which library?");
    EXPECT_EQ((*out)[0].header, "Library");
    EXPECT_FALSE((*out)[0].multi_select);
    ASSERT_EQ((*out)[0].options.size(), 2u);
    EXPECT_EQ((*out)[0].options[0].label, "axios");
}

// 场景:questions 长度越界(0 题或 5 题)应被拒,错误信息里包含 "questions"。
TEST(AskUserQuestionValidateTest, QuestionsLengthOutOfRangeRejected) {
    std::string err;
    auto empty = validate_ask_user_question_args(
        R"({"questions": []})", err);
    EXPECT_FALSE(empty.has_value());
    EXPECT_NE(err.find("questions"), std::string::npos) << err;

    err.clear();
    auto too_many = validate_ask_user_question_args(
        R"({"questions": [
            {"question":"A?", "header":"A",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"B?", "header":"B",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"C?", "header":"C",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"D?", "header":"D",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"E?", "header":"E",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]}
        ]})", err);
    EXPECT_FALSE(too_many.has_value());
    EXPECT_NE(err.find("questions"), std::string::npos) << err;
}

// 场景:某题 options 长度越界(1 或 5)应被拒,错误信息里包含 "options"。
TEST(AskUserQuestionValidateTest, OptionsLengthOutOfRangeRejected) {
    std::string err;
    auto too_few = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[{"label":"only","description":""}]
        }]})", err);
    EXPECT_FALSE(too_few.has_value());
    EXPECT_NE(err.find("options"), std::string::npos) << err;

    err.clear();
    auto too_many = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[
                {"label":"1","description":""},
                {"label":"2","description":""},
                {"label":"3","description":""},
                {"label":"4","description":""},
                {"label":"5","description":""}
            ]
        }]})", err);
    EXPECT_FALSE(too_many.has_value());
    EXPECT_NE(err.find("options"), std::string::npos) << err;
}

// 场景:两题 question 文本完全相同 → 被拒,错误信息里包含 "unique"。
TEST(AskUserQuestionValidateTest, DuplicateQuestionTextsRejected) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({"questions":[
            {"question":"Same?","header":"A",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"Same?","header":"B",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]}
        ]})", err);
    EXPECT_FALSE(out.has_value());
    EXPECT_NE(err.find("unique"), std::string::npos) << err;
}

// 场景:同一题里两个 option 的 label 完全相同 → 被拒,错误信息里
// 包含 "labels must be unique"(子串匹配)。
TEST(AskUserQuestionValidateTest, DuplicateOptionLabelsRejected) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[
                {"label":"same","description":"first"},
                {"label":"same","description":"second"}
            ]
        }]})", err);
    EXPECT_FALSE(out.has_value());
    EXPECT_NE(err.find("labels must be unique"), std::string::npos) << err;
}

// 场景:header 字符数 13(这里用 13 个中文字符,UTF-8 是 39 字节)→ 被拒,
// 错误信息包含 "header"。同时验证 header 字符数 12 的中文串是合法的
// (边界验证),避免把按字节数判断的实现误放过。
TEST(AskUserQuestionValidateTest, HeaderTooLongByCharCountRejected) {
    std::string err;
    auto too_long = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?",
            "header":"一二三四五六七八九十十一十二十三",
            "options":[{"label":"1","description":""},{"label":"2","description":""}]
        }]})", err);
    EXPECT_FALSE(too_long.has_value());
    EXPECT_NE(err.find("header"), std::string::npos) << err;

    err.clear();
    // 边界 12 字符:10 个 CJK + 2 个 ASCII,共 12 codepoints。这里故意混合 CJK
    // 与 ASCII,避免因代码 bug 按字节算时,全 CJK 字符串误被放过(30 bytes)。
    auto boundary = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?",
            "header":"一二三四五六七八九十AB",
            "options":[{"label":"1","description":""},{"label":"2","description":""}]
        }]})", err);
    EXPECT_TRUE(boundary.has_value()) << err;
}

// 场景:option 里出现 preview 字符串字段 → 校验通过,preview 不进入
// 返回结构。(AskOption 本身不持有 preview —— 校验层吞掉即可。)
TEST(AskUserQuestionValidateTest, PreviewFieldIsAcceptedButIgnored) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[
                {"label":"a","description":"d","preview":"<pre>ignored</pre>"},
                {"label":"b","description":"d"}
            ]
        }]})", err);
    ASSERT_TRUE(out.has_value()) << err;
    // AskOption 结构上没有 preview 字段 —— 编译即证明了"ignore";
    // 这里额外确认返回值里两个 option 都齐整。
    ASSERT_EQ((*out)[0].options.size(), 2u);
}

// 场景:format_ask_answers 单题单答,拼接与上游一致。
TEST(AskUserQuestionFormatTest, SingleQuestionSingleAnswer) {
    std::vector<std::string> order{"Which library?"};
    std::map<std::string, std::string> ans{{"Which library?", "axios"}};
    EXPECT_EQ(format_ask_answers(order, ans),
              "User has answered your questions: \"Which library?\"=\"axios\"");
}

// 场景:两题、第二题为 multi-select(调用方已经把多个 label 用 ", "
// 拼成单字符串),format 保持顺序 + 分隔符。
TEST(AskUserQuestionFormatTest, MultiQuestionWithMultiSelect) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "axios"},
        {"Q2?", "TypeScript, Prettier"}
    };
    EXPECT_EQ(format_ask_answers(order, ans),
              "User has answered your questions: \"Q1?\"=\"axios\", "
              "\"Q2?\"=\"TypeScript, Prettier\"");
}

// 场景:答案里含 `"` —— format 不做转义(和 claudecodehaha 同行为,
// 作为已记录的已知现象)。此 TEST 把未转义的 `"` 硬写进期望字符串里。
TEST(AskUserQuestionFormatTest, QuoteInAnswerIsNotEscaped) {
    std::vector<std::string> order{"Quote?"};
    std::map<std::string, std::string> ans{{"Quote?", "He said \"hi\""}};
    std::string out = format_ask_answers(order, ans);
    EXPECT_EQ(out,
              "User has answered your questions: \"Quote?\"=\"He said \"hi\"\"");
    // 额外断言:原样出现 3 对以上未转义双引号(Q/A 各一对 + 答案内 2 个 = 6)。
    EXPECT_GE(std::count(out.begin(), out.end(), '"'), 6);
}

// 场景:拒绝路径固定 ToolResult —— success=false 且 output 精确匹配。
TEST(AskUserQuestionRejectedTest, ConstantRejectedResult) {
    auto r = make_rejected_ask_result();
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.output, "[Error] User declined to answer questions.");
}
