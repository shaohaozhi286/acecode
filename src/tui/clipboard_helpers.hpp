#pragma once
// Clipboard and attachment helper functions extracted from main.cpp.
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <ftxui/component/event.hpp>
#include "tui_state.hpp"
#include "utils/clipboard.hpp"

namespace acecode { namespace tui {

void set_transient_status_line_locked(TuiState& state, const std::string& message);
std::string clipboard_paste_status_message(acecode::ClipboardTextReadResult::Status status);
std::string clipboard_image_status_message(acecode::ClipboardImageReadResult::Status status);
std::string clipboard_copy_status_message(acecode::ClipboardTextWriteResult::Status status);
bool is_alt_v_event(const ftxui::Event& event);
bool is_alt_a_event(const ftxui::Event& event);
std::string attachment_name_from_json(const nlohmann::json& attachment);
std::string display_prompt_with_attachments(const std::string& prompt,
                                            const std::vector<nlohmann::json>& attachments);
UserInput build_user_input_with_attachments(const std::string& prompt,
                                            const std::string& display_text,
                                            const std::vector<nlohmann::json>& attachments);

}} // namespace acecode::tui
