#include "models_command.hpp"

#include "../provider/models_dev_registry.hpp"
#include "../utils/models_dev_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <mutex>
#include <sstream>

namespace acecode {

namespace {

std::string source_kind_label(RegistrySource::Kind k) {
    switch (k) {
        case RegistrySource::Kind::Bundled:      return "bundled";
        case RegistrySource::Kind::UserOverride: return "user_override";
        case RegistrySource::Kind::Network:      return "network";
        case RegistrySource::Kind::Empty:        return "empty";
    }
    return "unknown";
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

void split_args(const std::string& args, std::string& sub, std::string& rest) {
    sub.clear();
    rest.clear();
    std::string trimmed = trim(args);
    if (trimmed.empty()) return;
    size_t sp = trimmed.find_first_of(" \t");
    if (sp == std::string::npos) {
        sub = trimmed;
    } else {
        sub = trimmed.substr(0, sp);
        rest = trim(trimmed.substr(sp + 1));
    }
}

int days_since_iso8601(const std::string& iso) {
    if (iso.size() < 10) return -1;
    std::tm tm{};
    if (sscanf(iso.c_str(), "%4d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
        return -1;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
#ifdef _WIN32
    time_t t = _mkgmtime(&tm);
#else
    time_t t = timegm(&tm);
#endif
    if (t == static_cast<time_t>(-1)) return -1;
    auto diff = std::chrono::system_clock::now() - std::chrono::system_clock::from_time_t(t);
    return static_cast<int>(std::chrono::duration_cast<std::chrono::hours>(diff).count() / 24);
}

void emit(CommandContext& ctx, const std::string& text) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.conversation.push_back({"system", text, false});
    ctx.state.chat_follow_tail = true;
}

void cmd_info(CommandContext& ctx, const std::string& /*rest*/) {
    auto registry = current_registry();
    const auto& src = current_registry_source();

    std::ostringstream oss;
    oss << "models.dev registry:\n"
        << "  source:    " << source_kind_label(src.kind);
    if (!src.path_or_url.empty()) {
        oss << "\n  location:  " << src.path_or_url;
    }

    if (registry && registry->is_object()) {
        size_t provider_count = 0;
        size_t model_count = 0;
        for (auto it = registry->begin(); it != registry->end(); ++it) {
            if (!it->is_object()) continue;
            ++provider_count;
            auto m_it = it->find("models");
            if (m_it == it->end()) continue;
            if (m_it->is_object()) model_count += m_it->size();
            else if (m_it->is_array()) model_count += m_it->size();
        }
        oss << "\n  providers: " << provider_count
            << "\n  models:    " << model_count;
    }

    if (src.manifest && src.manifest->is_object()) {
        const auto& m = *src.manifest;
        if (m.contains("upstream_remote") && m["upstream_remote"].is_string()) {
            oss << "\n  upstream:        " << m["upstream_remote"].get<std::string>();
        }
        if (m.contains("content_sha256") && m["content_sha256"].is_string()) {
            std::string sha = m["content_sha256"].get<std::string>();
            if (sha.size() > 12) sha = sha.substr(0, 12) + "...";
            oss << "\n  content_sha256:  " << sha;
        }
        if (m.contains("upstream_commit") && m["upstream_commit"].is_string()) {
            // Older manifests (<v2) recorded an upstream git commit; still show it
            // verbatim if present so operators can cross-reference legacy snapshots.
            oss << "\n  upstream_commit: " << m["upstream_commit"].get<std::string>();
        }
        if (m.contains("generated_at") && m["generated_at"].is_string()) {
            std::string ga = m["generated_at"].get<std::string>();
            oss << "\n  generated_at:    " << ga;
            int days = days_since_iso8601(ga);
            if (days >= 0) oss << "  (" << days << " days ago)";
        }
    }

    emit(ctx, oss.str());
}

void cmd_refresh(CommandContext& ctx, const std::string& rest) {
    bool with_network = (trim(rest) == "--network");
    if (with_network) {
        bool ok = refresh_registry_from_network();
        emit(ctx, ok ? "models.dev registry refreshed from network."
                     : "models.dev network refresh failed; existing snapshot preserved.");
    } else {
        // Reload from disk (catalog cache will rebuild on next access).
        // Need argv0_dir — but at runtime in TUI we don't have it; pass empty
        // and rely on env var / system path.
        reload_registry_from_disk(ctx.config, "");
        emit(ctx, "models.dev registry reloaded from disk.");
    }
}

void cmd_lookup(CommandContext& ctx, const std::string& rest) {
    std::string model_id = trim(rest);
    if (model_id.empty()) {
        emit(ctx, "Usage: /models lookup <model-id>");
        return;
    }
    auto registry = current_registry();
    if (!registry || registry->empty()) {
        emit(ctx, "No models.dev registry loaded.");
        return;
    }

    // Search across every provider.
    for (auto it = registry->begin(); it != registry->end(); ++it) {
        if (!it->is_object()) continue;
        auto models_it = it->find("models");
        if (models_it == it->end()) continue;
        if (models_it->is_object()) {
            auto m = models_it->find(model_id);
            if (m != models_it->end()) {
                std::ostringstream oss;
                oss << "provider: " << it.key() << "\n" << m->dump(2);
                emit(ctx, oss.str());
                return;
            }
        }
        if (models_it->is_array()) {
            for (const auto& m : *models_it) {
                if (m.is_object() && m.contains("id") && m["id"].is_string() &&
                    m["id"].get<std::string>() == model_id) {
                    std::ostringstream oss;
                    oss << "provider: " << it.key() << "\n" << m.dump(2);
                    emit(ctx, oss.str());
                    return;
                }
            }
        }
    }

    emit(ctx, "Model id '" + model_id + "' not found in registry.");
}

void cmd_models(CommandContext& ctx, const std::string& args) {
    std::string sub, rest;
    split_args(args, sub, rest);

    if (sub.empty() || sub == "info") {
        cmd_info(ctx, rest);
    } else if (sub == "refresh") {
        cmd_refresh(ctx, rest);
    } else if (sub == "lookup") {
        cmd_lookup(ctx, rest);
    } else {
        emit(ctx,
             "Unknown subcommand. Try: /models info | /models refresh [--network] | /models lookup <id>");
    }
}

} // namespace

void register_models_command(CommandRegistry& registry) {
    registry.register_command({"models",
                               "Inspect bundled models.dev registry (info, refresh, lookup)",
                               cmd_models});
}

} // namespace acecode
