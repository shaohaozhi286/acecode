#include "lsp_frame.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::lsp {
namespace {

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

std::string trim_ascii(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
    return s.substr(begin, end - begin);
}

// 解析头部块(不含结尾空行),取 Content-Length。返回 -1 = 头部非法。
long long parse_content_length(const std::string& header_block) {
    long long content_length = -1;
    std::size_t pos = 0;
    while (pos < header_block.size()) {
        std::size_t eol = header_block.find('\n', pos);
        std::string line = header_block.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? header_block.size() : eol + 1;
        line = trim_ascii(line);
        if (line.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) return -1;
        const std::string name = trim_ascii(line.substr(0, colon));
        const std::string value = trim_ascii(line.substr(colon + 1));
        if (iequals(name, "content-length")) {
            if (value.empty() ||
                !std::all_of(value.begin(), value.end(),
                             [](unsigned char c) { return std::isdigit(c); })) {
                return -1;
            }
            try {
                content_length = std::stoll(value);
            } catch (...) {
                return -1;
            }
        }
        // Content-Type 等其它头忽略。
    }
    return content_length;
}

} // namespace

std::string encode_frame(const nlohmann::json& message) {
    const std::string body = message.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool LspFrameParser::feed(const char* data, std::size_t len,
                          const std::function<void(nlohmann::json&&)>& on_message) {
    if (broken_) return false;
    buffer_.append(data, len);

    for (;;) {
        if (pending_body_bytes_ < 0) {
            // 找头部结束符。兼容严格 \r\n\r\n 与宽松 \n\n(个别 server 实现不规范)。
            std::size_t header_end = buffer_.find("\r\n\r\n");
            std::size_t body_begin;
            if (header_end != std::string::npos) {
                body_begin = header_end + 4;
            } else {
                header_end = buffer_.find("\n\n");
                if (header_end == std::string::npos) {
                    // 头部尚未到齐。防御:头部本身不应超过 4KB。
                    if (buffer_.size() > 4096) {
                        broken_ = true;
                        LOG_WARN("[lsp] frame header exceeds 4KB without terminator; stream broken");
                        return false;
                    }
                    return true;
                }
                body_begin = header_end + 2;
            }

            const long long content_length =
                parse_content_length(buffer_.substr(0, header_end));
            if (content_length < 0 ||
                static_cast<std::size_t>(content_length) > kMaxFrameBodyBytes) {
                broken_ = true;
                LOG_WARN("[lsp] invalid or oversized Content-Length header; stream broken");
                return false;
            }
            buffer_.erase(0, body_begin);
            pending_body_bytes_ = content_length;
        }

        if (buffer_.size() < static_cast<std::size_t>(pending_body_bytes_)) {
            return true; // body 未到齐,等下一次 feed
        }

        std::string body = buffer_.substr(0, static_cast<std::size_t>(pending_body_bytes_));
        buffer_.erase(0, static_cast<std::size_t>(pending_body_bytes_));
        pending_body_bytes_ = -1;

        nlohmann::json message = nlohmann::json::parse(body, nullptr, false);
        if (message.is_discarded()) {
            LOG_WARN("[lsp] dropped malformed JSON frame (" +
                     std::to_string(body.size()) + " bytes)");
            continue;
        }
        on_message(std::move(message));
    }
}

} // namespace acecode::lsp
