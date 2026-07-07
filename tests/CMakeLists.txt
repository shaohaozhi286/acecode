find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE ACECODE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp
)

add_executable(acecode_unit_tests ${ACECODE_TEST_SOURCES})

target_link_libraries(acecode_unit_tests PRIVATE
    acecode_testable
    acecode_desktop_support
    acecode_native_bridge_support
    ftxui::screen
    ftxui::dom
    ftxui::component
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(acecode_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# 测试辅助 binary: 用于 SessionConcurrencyE2E 测试 std::system 起两个子进程
# 验证同 cwd 下多 session 写 canonical 文件。独立 main(),不参与
# gtest_discover_tests。链 acecode_testable 拿 SessionManager。
add_executable(concurrent_session_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon/helpers/concurrent_session_writer.cpp
)
target_link_libraries(concurrent_session_writer PRIVATE
    acecode_testable
    acecode_native_bridge_support
    # acecode_testable 里 src/tool/ask_user_question_tool.cpp 等 TU 引用了
    # ftxui::Event,所以即使 helper main 自己只调 SessionManager,链接器也要
    # 拉 ftxui 才能 resolve。匹配 acecode_unit_tests 的 link 列表。
    ftxui::screen
    ftxui::dom
    ftxui::component
)
target_include_directories(concurrent_session_writer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
add_dependencies(acecode_unit_tests concurrent_session_writer)

# 测试辅助 MCP stdio server: 用于 McpManager 异步启动测试走真实
# stdio JSON-RPC 路径,避免依赖外部 node/python。
add_executable(mcp_stdio_test_server
    ${CMAKE_CURRENT_SOURCE_DIR}/tool/helpers/mcp_stdio_test_server.cpp
)
target_link_libraries(mcp_stdio_test_server PRIVATE
    nlohmann_json::nlohmann_json
)
add_dependencies(acecode_unit_tests mcp_stdio_test_server)

# 测试辅助 LSP stdio server: LspClient 集成测试走真实进程/管道/
# Content-Length 分帧路径,不依赖外部 node / 真实 clangd。
add_executable(fake_lsp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp/helpers/fake_lsp_server.cpp
)
target_link_libraries(fake_lsp_server PRIVATE
    nlohmann_json::nlohmann_json
)
add_dependencies(acecode_unit_tests fake_lsp_server)

# 注入 helper 绝对路径给测试代码(避免 ctest cwd 不确定问题)
target_compile_definitions(acecode_unit_tests PRIVATE
    ACECODE_CONCURRENT_SESSION_WRITER_PATH="$<TARGET_FILE:concurrent_session_writer>"
    ACECODE_MCP_STDIO_TEST_SERVER_PATH="$<TARGET_FILE:mcp_stdio_test_server>"
    ACECODE_FAKE_LSP_SERVER_PATH="$<TARGET_FILE:fake_lsp_server>"
)

# winpty spike/冒烟测试需要 winpty-agent.exe 在单测 exe 同目录
# (findAgentProgram 未设置覆盖路径时的 fallback 查找位置)。
if(WIN32)
    add_custom_command(TARGET acecode_unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:winpty-agent>
            $<TARGET_FILE_DIR:acecode_unit_tests>
    )
    add_dependencies(acecode_unit_tests winpty-agent)
endif()

include(GoogleTest)
gtest_discover_tests(acecode_unit_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)
