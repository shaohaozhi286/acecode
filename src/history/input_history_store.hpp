// 持久化输入历史：以工作目录为单位滚动保留最近 N 条用户提交。
// 文件布局：<project_dir>/input_history.jsonl，每行一个独立 JSON 对象
// { "text": "<完整字符串，含 ! 等 mode 前缀>" }。
// 设计 rationale 见 openspec/changes/persistent-input-history/design.md。
#pragma once

#include <string>
#include <vector>

namespace acecode {

class InputHistoryStore {
public:
    // 组合文件路径：<project_dir>/input_history.jsonl
    static std::string file_path(const std::string& project_dir);

    // 从磁盘加载历史。按从旧到新的顺序返回。
    // - 文件不存在 / 权限错误 → 返回空 vector，不抛异常
    // - 单行 JSON 解析失败 / 缺 text 字段 → 跳过并写 warning 日志
    // - 返回结果行数不保证 <= max_entries（来源数据可能未截断，调用方 load 后若越
    //   限会在下一次 append 时自动修正，或调用方也可在载入后自行裁剪）
    static std::vector<std::string> load(const std::string& path);

    // 追加一条历史到磁盘。entry 字符串原样写入 JSON 的 text 字段（含 ! 前缀等）。
    // 空字符串或 trim 后为空会被忽略（不抛错、不写日志）。
    // 若追加后文件行数 > max_entries，自动触发一次 head 截断（保留最近 max_entries
    // 条），采用「临时文件 + rename」保证原子性。
    // 任何 IO / JSON 异常都被吞掉并写 error 日志；永远不上抛。
    static void append(const std::string& path, const std::string& entry, int max_entries);

    // 清空磁盘历史。文件不存在视为成功；删除失败仅写 error 日志。
    static void clear(const std::string& path);
};

} // namespace acecode
