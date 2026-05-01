#pragma once

// confirm_question.hpp
// 工具确认 overlay 用的标题生成器。把 (tool_name, arguments_json) 映射成
// 一句给用户看的疑问句,例如 "Do you want to run this command?",由
// main.cpp 的 confirm_overlay_element 渲染。
//
// 单独抽出来是为了:
//   1) 在没有 FTXUI 依赖的前提下做 nlohmann::json 解析,这样能编进
//      acecode_testable OBJECT library 并写 ctest;
//   2) 把字符串拼接和 fallback 逻辑(unknown tool / 坏 JSON / 缺字段)
//      集中到一处,避免 main.cpp 的 render 路径里堆字符串。

#include <string>

namespace acecode::tui {

// 根据 tool_name 和 arguments_json 生成确认 overlay 顶部的疑问句。
// 永不抛异常 —— JSON 解析失败 / 缺字段 / 未知工具一律走通用分支
// "Do you want to use <tool_name>?"。
std::string build_confirm_question(const std::string& tool_name,
                                   const std::string& arguments_json);

} // namespace acecode::tui
