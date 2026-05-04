#include "http.hpp"

#include "../network/proxy_resolver.hpp"

#include <cpr/cpr.h>
#include <fstream>

namespace acecode::upgrade {

HttpTextResult fetch_text(const std::string& url, int timeout_ms) {
    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Accept", "application/json"}, {"User-Agent", "acecode-updater"}},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{timeout_ms});

    HttpTextResult out;
    out.status_code = r.status_code;
    out.body = std::move(r.text);
    if (r.error.code != cpr::ErrorCode::OK) {
        out.error = r.error.message;
    }
    return out;
}

DownloadResult download_to_file(const std::string& url,
                                const std::filesystem::path& output_path,
                                int timeout_ms) {
    DownloadResult out;
    std::ofstream ofs(output_path, std::ios::binary);
    if (!ofs) {
        out.error = "failed to open output file: " + output_path.string();
        return out;
    }

    auto write_cb = cpr::WriteCallback{
        [&](const std::string_view data, intptr_t) -> bool {
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!ofs) return false;
            out.bytes_written += static_cast<std::uintmax_t>(data.size());
            return true;
        }
    };

    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Accept", "application/zip"}, {"User-Agent", "acecode-updater"}},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{timeout_ms},
        write_cb);

    out.status_code = r.status_code;
    if (r.error.code != cpr::ErrorCode::OK) {
        out.error = r.error.message;
    }
    return out;
}

} // namespace acecode::upgrade
