#include "upgrade/apply.hpp"
#include "upgrade/package.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

using namespace acecode::upgrade;

namespace {

fs::path temp_root(const std::string& name) {
    auto p = fs::temp_directory_path() /
             (name + "-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

#ifdef _WIN32
class ProcessGuard {
public:
    ~ProcessGuard() { stop(); }

    void stop() {
        if (process_) {
            ::TerminateProcess(process_, 0);
            ::WaitForSingleObject(process_, 5000);
            ::CloseHandle(process_);
            process_ = nullptr;
        }
        if (thread_) {
            ::CloseHandle(thread_);
            thread_ = nullptr;
        }
    }

    HANDLE* process_address() { return &process_; }
    HANDLE* thread_address() { return &thread_; }
    HANDLE process() const { return process_; }

private:
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
};
#endif

} // namespace

TEST(UpgradeApply, BuildsAndParsesRunnerArguments) {
    ApplyOptions opts;
    opts.parent_pid = 1234;
    opts.staging_dir = "C:/Temp/staging dir";
    opts.install_dir = "C:/Program Files/ACECode";
    opts.backup_dir = "C:/Temp/backup";

    auto args = build_apply_runner_args(opts);
    ASSERT_EQ(args.front(), "--apply-update");

    std::string err;
    std::vector<std::string> tail(args.begin() + 1, args.end());
    auto parsed = parse_apply_runner_args(tail, &err);
    ASSERT_TRUE(parsed.has_value()) << err;
    EXPECT_EQ(parsed->parent_pid, opts.parent_pid);
    EXPECT_EQ(parsed->staging_dir, opts.staging_dir);
    EXPECT_EQ(parsed->install_dir, opts.install_dir);
    EXPECT_EQ(parsed->backup_dir, opts.backup_dir);
    EXPECT_EQ(quote_command_arg("C:/Program Files/ACECode/acecode.exe"),
              "\"C:/Program Files/ACECode/acecode.exe\"");

    auto runner = make_runner_path(1234, "C:/Program Files/ACECode");
    EXPECT_EQ(runner.parent_path().filename(), ".acecode-update-runner");
#ifdef _WIN32
    EXPECT_EQ(runner.filename(), "acecode-update-runner-1234.exe");
#else
    EXPECT_EQ(runner.filename(), "acecode-update-runner-1234");
#endif
}

TEST(UpgradeApply, AppliesStagedUpdateAndKeepsBackup) {
    fs::path root = temp_root("acecode-apply");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";

    write_file(install / "acecode.exe", "old exe");
    write_file(install / "old.txt", "old unrelated");
    write_file(staging / "acecode.exe", "new exe");
    write_file(staging / "share" / "asset.txt", "asset");

    std::string err;
    ASSERT_TRUE(apply_staged_update(staging, install, backup, "windows-x64", &err)) << err;
    EXPECT_EQ(read_file(install / "acecode.exe"), "new exe");
    EXPECT_EQ(read_file(install / "share" / "asset.txt"), "asset");
    EXPECT_EQ(read_file(install / "old.txt"), "old unrelated");
    EXPECT_EQ(read_file(backup / "acecode.exe"), "old exe");
    EXPECT_FALSE(fs::exists(backup / "old.txt"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(UpgradeApply, KeepsRunnerDirectoryInInstallDir) {
    fs::path root = temp_root("acecode-apply-runner-dir");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";

    write_file(install / "old.txt", "old");
    write_file(install / ".acecode-update-runner" / "acecode-update-runner-1234.exe",
               "runner");
    write_file(staging / "acecode.exe", "new exe");

    std::string err;
    ASSERT_TRUE(apply_staged_update(staging, install, backup, "windows-x64", &err)) << err;
    EXPECT_EQ(read_file(install / "acecode.exe"), "new exe");
    EXPECT_EQ(read_file(install / "old.txt"), "old");
    EXPECT_EQ(read_file(install / ".acecode-update-runner" /
                        "acecode-update-runner-1234.exe"), "runner");
    EXPECT_FALSE(fs::exists(backup / ".acecode-update-runner"));
    EXPECT_FALSE(fs::exists(backup / "old.txt"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

#ifdef _WIN32
TEST(UpgradeApply, ReplacesExecutableWhileItIsRunning) {
    fs::path root = temp_root("acecode-apply-running-exe");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";
    fs::create_directories(install);

    std::array<wchar_t, MAX_PATH> system_dir{};
    const UINT system_dir_length = ::GetSystemDirectoryW(
        system_dir.data(), static_cast<UINT>(system_dir.size()));
    ASSERT_GT(system_dir_length, 0U);
    ASSERT_LT(system_dir_length, system_dir.size());

    const fs::path running_exe = install / "acecode.exe";
    std::error_code ec;
    fs::copy_file(fs::path(system_dir.data()) / "PING.EXE", running_exe,
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    write_file(staging / "acecode.exe", "new exe");

    std::wstring command_line = L"\"" + running_exe.wstring() +
                                L"\" -t 127.0.0.1";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    ASSERT_TRUE(::CreateProcessW(
        running_exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_info))
        << "CreateProcessW error " << ::GetLastError();

    ProcessGuard process;
    *process.process_address() = process_info.hProcess;
    *process.thread_address() = process_info.hThread;
    ASSERT_EQ(::WaitForSingleObject(process.process(), 0), WAIT_TIMEOUT);

    std::string err;
    ASSERT_TRUE(apply_staged_update(staging, install, backup,
                                    "windows-x64", &err))
        << err;
    EXPECT_EQ(read_file(running_exe), "new exe");
    EXPECT_TRUE(fs::is_regular_file(backup / "acecode.exe"));
    EXPECT_GT(fs::file_size(backup / "acecode.exe"), 0U);

    process.stop();
    fs::remove_all(root, ec);
}
#endif

TEST(UpgradeApply, ReplacesNestedPackageFilesWithoutMovingUnrelatedSiblings) {
    fs::path root = temp_root("acecode-apply-nested");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";

    write_file(install / "acecode.exe", "old exe");
    write_file(install / "share" / "asset.txt", "old asset");
    write_file(install / "share" / "local-only.txt", "keep me");
    write_file(install / "apikey-generator-rs" / "locked-sentinel.txt", "do not touch");
    write_file(staging / "acecode.exe", "new exe");
    write_file(staging / "share" / "asset.txt", "new asset");

    std::string err;
    ASSERT_TRUE(apply_staged_update(staging, install, backup, "windows-x64", &err)) << err;
    EXPECT_EQ(read_file(install / "acecode.exe"), "new exe");
    EXPECT_EQ(read_file(install / "share" / "asset.txt"), "new asset");
    EXPECT_EQ(read_file(install / "share" / "local-only.txt"), "keep me");
    EXPECT_EQ(read_file(install / "apikey-generator-rs" / "locked-sentinel.txt"),
              "do not touch");
    EXPECT_EQ(read_file(backup / "acecode.exe"), "old exe");
    EXPECT_EQ(read_file(backup / "share" / "asset.txt"), "old asset");
    EXPECT_FALSE(fs::exists(backup / "share" / "local-only.txt"));
    EXPECT_FALSE(fs::exists(backup / "apikey-generator-rs"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(UpgradeApply, InvalidStageDoesNotModifyInstall) {
    fs::path root = temp_root("acecode-apply-invalid");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";

    write_file(install / "old.txt", "old");
    write_file(staging / "readme.txt", "missing exe");

    std::string err;
    EXPECT_FALSE(apply_staged_update(staging, install, backup, "windows-x64", &err));
    EXPECT_EQ(read_file(install / "old.txt"), "old");

    std::error_code ec;
    fs::remove_all(root, ec);
}
