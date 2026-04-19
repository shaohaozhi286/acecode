#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <aclapi.h>
#  include <sddl.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace acecode {

// Atomically write `content` to `path`. Writes to a sibling temp file first,
// then renames. Returns true on success. Caller can opt into restrictive
// permissions for sensitive payloads (token, keys).
inline bool atomic_write_file(const std::string& path,
                              const std::string& content,
                              bool restrict_permissions = false) {
    namespace fs = std::filesystem;
    fs::path target(path);
    fs::path tmp = target;
    tmp += ".tmp";

    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) return false;
    }

    if (restrict_permissions) {
#ifndef _WIN32
        // 0600
        if (::chmod(tmp.string().c_str(), S_IRUSR | S_IWUSR) != 0) {
            std::error_code rmec;
            fs::remove(tmp, rmec);
            return false;
        }
#else
        // Restrict ACL to current user only. Best-effort: failure does not
        // abort the write since the file is still on a per-user profile path
        // by convention. Future hardening could fail-hard here.
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD len = 0;
            ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
            if (len > 0) {
                std::string buf(len, '\0');
                if (::GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
                    PSID user_sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
                    EXPLICIT_ACCESSA ea{};
                    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
                    ea.grfAccessMode = SET_ACCESS;
                    ea.grfInheritance = NO_INHERITANCE;
                    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
                    ea.Trustee.ptstrName = static_cast<LPSTR>(user_sid);
                    PACL acl = nullptr;
                    if (::SetEntriesInAclA(1, &ea, nullptr, &acl) == ERROR_SUCCESS && acl) {
                        ::SetNamedSecurityInfoA(
                            const_cast<LPSTR>(tmp.string().c_str()),
                            SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, acl, nullptr);
                        ::LocalFree(acl);
                    }
                }
            }
            ::CloseHandle(token);
        }
#endif
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // rename across devices, or other failure: try copy+remove fallback,
        // then bubble up failure. Keep tmp present so caller can inspect.
        return false;
    }
    return true;
}

} // namespace acecode
