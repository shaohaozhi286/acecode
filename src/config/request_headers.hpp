#pragma once

#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

using RequestHeaders = std::map<std::string, std::string>;

std::optional<RequestHeaders> parse_request_headers_json(const nlohmann::json& node,
                                                         const std::string& context,
                                                         std::string& err);

bool validate_request_headers(const RequestHeaders& headers,
                              std::string& err,
                              bool allow_authorization = true);

std::optional<RequestHeaders> resolve_request_headers(const RequestHeaders& headers,
                                                      std::string& err);

} // namespace acecode
