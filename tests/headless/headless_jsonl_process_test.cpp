#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace {

class ChildWithPipes {
public:
    ~ChildWithPipes() {
        close_input();
        close_handle(stdout_read_);
        if (process_ != nullptr) {
            if (::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
                ::TerminateProcess(process_, 255);
                ::WaitForSingleObject(process_, 5000);
            }
            ::CloseHandle(process_);
        }
    }

    bool start(const std::string& mode, std::string& error) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE child_stdout = nullptr;
        HANDLE child_stdin = nullptr;
        if (!::CreatePipe(&stdout_read_, &child_stdout, &sa, 0)) {
            error = "CreatePipe(stdout) failed";
            return false;
        }
        if (!::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0)) {
            error = "SetHandleInformation(stdout) failed";
            close_handle(child_stdout);
            return false;
        }
        if (!::CreatePipe(&child_stdin, &stdin_write_, &sa, 0)) {
            error = "CreatePipe(stdin) failed";
            close_handle(child_stdout);
            return false;
        }
        if (!::SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
            error = "SetHandleInformation(stdin) failed";
            close_handle(child_stdout);
            close_handle(child_stdin);
            return false;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_stdin;
        startup.hStdOutput = child_stdout;
        startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION process_info{};
        const std::filesystem::path executable(ACECODE_HEADLESS_JSONL_DRIVER_PATH);
        std::wstring command = L"\"" + executable.wstring() + L"\" " +
            std::wstring(mode.begin(), mode.end());
        const BOOL created = ::CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_info);
        close_handle(child_stdout);
        close_handle(child_stdin);
        if (!created) {
            error = "CreateProcessW failed: " + std::to_string(::GetLastError());
            return false;
        }
        process_ = process_info.hProcess;
        ::CloseHandle(process_info.hThread);
        return true;
    }

    bool write_input(const std::string& value) {
        DWORD written = 0;
        return stdin_write_ != nullptr &&
            ::WriteFile(stdin_write_, value.data(),
                        static_cast<DWORD>(value.size()), &written, nullptr) &&
            written == value.size();
    }

    void close_input() { close_handle(stdin_write_); }
    void close_output() { close_handle(stdout_read_); }

    bool running() const {
        return process_ != nullptr &&
            ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    bool wait(DWORD timeout_ms, DWORD& exit_code) const {
        if (process_ == nullptr ||
            ::WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) {
            return false;
        }
        return ::GetExitCodeProcess(process_, &exit_code) != FALSE;
    }

    bool read_until_newline(std::string& output, DWORD timeout_ms) const {
        return read_available(output, timeout_ms, true);
    }

    bool read_to_eof(std::string& output, DWORD timeout_ms) const {
        return read_available(output, timeout_ms, false);
    }

private:
    static void close_handle(HANDLE& handle) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
        handle = nullptr;
    }

    bool read_available(std::string& output, DWORD timeout_ms,
                        bool stop_at_newline) const {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available = 0;
            if (stdout_read_ == nullptr ||
                !::PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available,
                                 nullptr)) {
                return !stop_at_newline && !running();
            }
            if (available > 0) {
                std::vector<char> buffer(
                    static_cast<std::size_t>(available > 64 * 1024
                                                 ? 64 * 1024
                                                 : available));
                DWORD read = 0;
                if (!::ReadFile(stdout_read_, buffer.data(),
                                static_cast<DWORD>(buffer.size()), &read,
                                nullptr)) {
                    return false;
                }
                output.append(buffer.data(), read);
                if (stop_at_newline && output.find('\n') != std::string::npos) {
                    return true;
                }
                continue;
            }
            if (!running()) return !stop_at_newline;
            ::Sleep(5);
        }
        return false;
    }

    HANDLE process_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stdin_write_ = nullptr;
};

std::vector<nlohmann::json> parse_jsonl(const std::string& text) {
    std::vector<nlohmann::json> records;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        if (end == std::string::npos) break;
        auto line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) records.push_back(nlohmann::json::parse(line));
        begin = end + 1;
    }
    return records;
}

TEST(HeadlessJsonlProcess, FlushesFirstRecordBeforeProducerContinues) {
    ChildWithPipes child;
    std::string error;
    ASSERT_TRUE(child.start("staged", error)) << error;

    std::string first_output;
    ASSERT_TRUE(child.read_until_newline(first_output, 5000));
    const auto first_records = parse_jsonl(first_output);
    ASSERT_EQ(first_records.size(), 1u);
    EXPECT_EQ(first_records[0].at("type"), "step_start");
    EXPECT_TRUE(child.running());

    ASSERT_TRUE(child.write_input("x"));
    child.close_input();
    std::string remaining;
    ASSERT_TRUE(child.read_to_eof(remaining, 5000));
    DWORD exit_code = 0;
    ASSERT_TRUE(child.wait(5000, exit_code));
    EXPECT_EQ(exit_code, 0u);

    const auto records = parse_jsonl(remaining);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].at("type"), "text");
    EXPECT_EQ(records[1].at("type"), "step_finish");
}

TEST(HeadlessJsonlProcess, SlowConsumerReceivesWholeOrderedRecords) {
    ChildWithPipes child;
    std::string error;
    ASSERT_TRUE(child.start("bulk", error)) << error;

    ::Sleep(100);
    EXPECT_TRUE(child.running());

    std::string output;
    ASSERT_TRUE(child.read_to_eof(output, 15000));
    DWORD exit_code = 0;
    ASSERT_TRUE(child.wait(5000, exit_code));
    ASSERT_EQ(exit_code, 0u);

    const auto records = parse_jsonl(output);
    ASSERT_EQ(records.size(), 256u);
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(records[static_cast<std::size_t>(i)].at("index"), i);
    }
}

TEST(HeadlessJsonlProcess, ClosedConsumerMakesProducerFail) {
    ChildWithPipes child;
    std::string error;
    ASSERT_TRUE(child.start("bulk", error)) << error;
    child.close_output();

    DWORD exit_code = 0;
    ASSERT_TRUE(child.wait(5000, exit_code));
    EXPECT_NE(exit_code, 0u);
}

} // namespace

#else

TEST(HeadlessJsonlProcess, RequiresWindowsPipeHarness) {
    GTEST_SKIP() << "Real-pipe harness is implemented for the Windows target.";
}

#endif
