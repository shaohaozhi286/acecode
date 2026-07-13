#include <gtest/gtest.h>

#include "tui/path_reference_input.hpp"
#include "tui_state.hpp"
#include "utils/encoding.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("acecode_tui_path_reference_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path / "src");
        std::ofstream(path / "src" / "main.cpp") << "int main(){}";
        fs::create_directories(path / "docs");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string cwd() const {
#ifdef _WIN32
        return acecode::wide_to_utf8(path.generic_wstring());
#else
        return path.generic_string();
#endif
    }
};

} // namespace

TEST(TuiPathReferenceInput, RefreshesBareAtAndKeepsDirectoriesFirst) {
    TempDir tmp;
    acecode::TuiState state;
    state.input_text = "@";
    state.input_cursor = 1;
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    ASSERT_TRUE(state.path_reference_active);
    ASSERT_GE(state.path_reference_items.size(), 2u);
    EXPECT_TRUE(state.path_reference_items[0].is_directory);
}

TEST(TuiPathReferenceInput, EnterReferencesDirectoryAndClosesDropdown) {
    TempDir tmp;
    acecode::TuiState state;
    state.input_text = "check @do later";
    state.input_cursor = 9;
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    ASSERT_TRUE(state.path_reference_active);
    ASSERT_TRUE(state.path_reference_items[state.path_reference_selected].is_directory);
    ASSERT_TRUE(acecode::tui::commit_path_reference_selection(state, false));
    EXPECT_EQ(state.input_text, "check @docs/  later");
    EXPECT_FALSE(state.path_reference_active);
}

TEST(TuiPathReferenceInput, TabEntersDirectoryAndRefreshesChildren) {
    TempDir tmp;
    acecode::TuiState state;
    state.input_text = "@sr";
    state.input_cursor = 3;
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    ASSERT_TRUE(acecode::tui::commit_path_reference_selection(state, true));
    EXPECT_EQ(state.input_text, "@src/");
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    ASSERT_EQ(state.path_reference_items.size(), 1u);
    EXPECT_EQ(state.path_reference_items[0].name, "main.cpp");
}

TEST(TuiPathReferenceInput, EscapeDismissesOnlyCurrentToken) {
    TempDir tmp;
    acecode::TuiState state;
    state.input_text = "@";
    state.input_cursor = 1;
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    acecode::tui::dismiss_path_reference_state(state);
    EXPECT_EQ(state.input_text, "@");
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    EXPECT_FALSE(state.path_reference_active);
    state.input_text += "s";
    state.input_cursor = state.input_text.size();
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    EXPECT_TRUE(state.path_reference_active);
}

TEST(TuiPathReferenceInput, SuppressesShellAndOverlayModes) {
    TempDir tmp;
    acecode::TuiState state;
    state.input_text = "@";
    state.input_cursor = 1;
    state.input_mode = acecode::InputMode::Shell;
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    EXPECT_FALSE(state.path_reference_active);
    state.input_mode = acecode::InputMode::Normal;
    state.ask_pending = true;
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    EXPECT_FALSE(state.path_reference_active);
}

TEST(TuiPathReferenceInput, SelectionMovementWrapsAndPreservesUtf8Cursor) {
    TempDir tmp;
    acecode::TuiState state;
    state.input_text = u8"请看 @s";
    state.input_cursor = state.input_text.size();
    acecode::tui::refresh_path_reference_state(state, tmp.cwd());
    ASSERT_FALSE(state.path_reference_items.empty());
    acecode::tui::move_path_reference_selection(state, -1);
    EXPECT_GE(state.path_reference_selected, 0);
    ASSERT_TRUE(acecode::tui::commit_path_reference_selection(state, false));
    EXPECT_LE(state.input_cursor, state.input_text.size());
    if (state.input_cursor < state.input_text.size()) {
        EXPECT_NE(static_cast<unsigned char>(state.input_text[state.input_cursor]) & 0xC0,
                  0x80);
    }
}
