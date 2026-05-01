#pragma once

#include <optional>

// 跨会话进程状态(once-only 提示标记等),持久化到 ~/.acecode/state.json。
// 跟 config.json 区分:config.json 是用户编辑面,state.json 是 ACECode 自己
// 写的运行时状态(同一个数据目录下,通过 paths::resolve_data_dir(RunMode)
// 得到)。
//
// 设计取舍见 openspec/changes/add-legacy-terminal-fallback/design.md
// Decision 4。本 helper 故意做得很轻 — 只支持 bool flag,key/value 一对一。
// 后续如果有需求再扩 string / int。

#include <string>

namespace acecode {

// 读 state.json 中 key 对应的 bool。文件不存在 / 损坏 / key 缺失 / 类型不符
// 都返回 false。永不抛异常。
bool read_state_flag(const std::string& key);

// 测试专用:覆盖 state.json 的解析路径。传空串清除覆盖,回到从
// resolve_data_dir(get_run_mode()) 计算的默认。生产代码不应调用。
void set_state_file_path_for_test(const std::string& path);

// 写 state.json:把 key 设为 value,保留其它已有 key。
// 文件损坏(非合法 JSON)时整体覆盖并写一条 LOG_WARN。
// 写失败(权限 / 只读盘等)只 LOG_WARN,不阻断调用方。
//
// 用法:write_state_flag("legacy_terminal_hint_shown", true);
void write_state_flag(const std::string& key, bool value);

// 联网搜索 region 缓存(参见 add-web-search-tool 的 RegionDetector)。
// state.json 中的存储格式:
//   { "web_search": { "region_detected": "global"|"cn",
//                       "region_detected_at_ms": <epoch ms> } }
struct WebSearchRegionCache {
    std::string region;          // "global" / "cn"
    long long detected_at_ms = 0;
};

// 读取缓存的 region。文件不存在 / 字段缺失 / region 不是 global|cn → nullopt。
// detected_at_ms 即使为 0 / 缺失也接受(只表示来源未知,region 仍可信)。
std::optional<WebSearchRegionCache> read_web_search_region_cache();

// 写缓存的 region。原子写,保留其他 key。region 不在 {global, cn} 时静默不写。
void write_web_search_region_cache(const WebSearchRegionCache& cache);

// 删除 web_search 缓存(/websearch refresh 用)。其它 key 保留。
void clear_web_search_region_cache();

} // namespace acecode
