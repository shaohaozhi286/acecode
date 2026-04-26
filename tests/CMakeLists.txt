find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE ACECODE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp
)

add_executable(acecode_unit_tests ${ACECODE_TEST_SOURCES})

target_link_libraries(acecode_unit_tests PRIVATE
    acecode_testable
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
# 验证 daemon + TUI 同 cwd 不撞 session 文件(任务 13.7)。独立 main(),不参与
# gtest_discover_tests。链 acecode_testable 拿 SessionManager,不需要 ftxui。
add_executable(concurrent_session_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon/helpers/concurrent_session_writer.cpp
)
target_link_libraries(concurrent_session_writer PRIVATE
    acecode_testable
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

# 注入 helper 绝对路径给测试代码(避免 ctest cwd 不确定问题)
target_compile_definitions(acecode_unit_tests PRIVATE
    ACECODE_CONCURRENT_SESSION_WRITER_PATH="$<TARGET_FILE:concurrent_session_writer>"
)

include(GoogleTest)
gtest_discover_tests(acecode_unit_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)
