cmake_minimum_required(VERSION 3.20)
project(webview_demo CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch latest webview/webview directly from GitHub
include(FetchContent)
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(webview)

add_executable(${PROJECT_NAME} WIN32 MACOSX_BUNDLE src/main.cpp)

# The webview library provides a target named 'webview' which automatically
# handles all the platform-specific linking (Edge/WebView2, WebKit, GTK)
target_link_libraries(${PROJECT_NAME} PRIVATE webview::core_static)

if(MSVC)
    # Treat our source as UTF-8 (we have Chinese comments).
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()
