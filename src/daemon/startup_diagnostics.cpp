#include "startup_diagnostics.hpp"

#include "../config/config.hpp"
#include "../utils/utf8_path.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::daemon {
namespace {

std::string local_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

} // namespace

void append_startup_diagnostic(const std::string& message) {
    std::error_code ec;
    fs::path run_dir = path_from_utf8(get_run_dir());
    fs::create_directories(run_dir, ec);
    if (ec) return;

    std::ofstream out(run_dir / "daemon-startup.log",
                      std::ios::binary | std::ios::app);
    if (!out.is_open()) return;
    out << local_timestamp() << " " << message << "\n";
}

} // namespace acecode::daemon
