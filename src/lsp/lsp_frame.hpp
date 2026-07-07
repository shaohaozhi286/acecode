#pragma once

// LSP 标准分帧:`Content-Length: N\r\n[其它头\r\n]\r\n<N 字节 UTF-8 body>`。
// LspFrameParser 是纯状态机,不做任何 IO —— reader 线程把读到的字节增量喂进来,
// 每凑齐一条完整消息回调一次。设计成纯类以便单测覆盖半包/粘包/非法头。

#include <cstddef>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode::lsp {

// 单帧 body 上限。LSP 消息(含 workspace symbol 全量结果)正常远小于此;
// 超限视为流损坏,防御恶意/失控 server 导致的无界内存增长。
inline constexpr std::size_t kMaxFrameBodyBytes = 128ull * 1024 * 1024;

std::string encode_frame(const nlohmann::json& message);

class LspFrameParser {
public:
    // 追加新到达的字节;每解析出一条完整 JSON 消息调用一次 on_message。
    // body 不是合法 JSON 时该帧被丢弃(不回调),流继续。
    // 返回 false = 流已损坏(头部非法 / Content-Length 缺失或超限),
    // 调用方应停止读取并关闭连接;此后再 feed 恒返回 false。
    bool feed(const char* data, std::size_t len,
              const std::function<void(nlohmann::json&&)>& on_message);

    bool broken() const { return broken_; }

private:
    std::string buffer_;
    // <0 = 正在等待完整头部;>=0 = 头部已解析,等 body 凑满这么多字节。
    long long pending_body_bytes_ = -1;
    bool broken_ = false;
};

} // namespace acecode::lsp
