#include "context_picker.hpp"

#ifdef _WIN32

#include "../utils/logger.hpp"

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <atomic>
#include <string>

namespace acecode::desktop {

namespace {

constexpr DWORD kSelectCurrentFolderButton = 0xACE1;
constexpr const wchar_t* kContextPickerTitle =
    L"\u6dfb\u52a0\u56fe\u7247\u3001\u6587\u4ef6\u6216\u6587\u4ef6\u5939";
constexpr const wchar_t* kSelectCurrentFolderLabel =
    L"\u9009\u62e9\u5f53\u524d\u6587\u4ef6\u5939";

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

std::optional<std::string> filesystem_path(IShellItem* item) {
    if (!item) return std::nullopt;
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || !raw_path) {
        return std::nullopt;
    }
    std::optional<std::string> result = wide_to_utf8(raw_path);
    ::CoTaskMemFree(raw_path);
    if (result->empty()) return std::nullopt;
    return result;
}

bool shell_item_is_folder(IShellItem* item) {
    if (!item) return false;
    SFGAOF attributes = 0;
    return SUCCEEDED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes)) &&
           (attributes & SFGAO_FOLDER) != 0 &&
           (attributes & SFGAO_FILESYSTEM) != 0;
}

class ContextPickerEvents final : public IFileDialogEvents,
                                  public IFileDialogControlEvents {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IFileDialogEvents)) {
            *object = static_cast<IFileDialogEvents*>(this);
        } else if (IsEqualIID(riid, IID_IFileDialogControlEvents)) {
            *object = static_cast<IFileDialogControlEvents*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*,
                                    FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) *response = FDESVR_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*,
                               FDE_OVERWRITE_RESPONSE* response) override {
        if (response) *response = FDEOR_DEFAULT;
        return S_OK;
    }

    IFACEMETHODIMP OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override {
        return S_OK;
    }

    IFACEMETHODIMP OnButtonClicked(IFileDialogCustomize* customize,
                                   DWORD control_id) override {
        if (control_id != kSelectCurrentFolderButton || !customize) return S_OK;

        IFileDialog* dialog = nullptr;
        if (FAILED(customize->QueryInterface(IID_PPV_ARGS(&dialog))) || !dialog) {
            return S_OK;
        }

        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetFolder(&item)) && item) {
            folder_path_ = filesystem_path(item);
            item->Release();
        }
        if (folder_path_) dialog->Close(S_OK);
        dialog->Release();
        return S_OK;
    }

    IFACEMETHODIMP OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override {
        return S_OK;
    }
    IFACEMETHODIMP OnControlActivating(IFileDialogCustomize*, DWORD) override {
        return S_OK;
    }

    const std::optional<std::string>& folder_path() const { return folder_path_; }

private:
    ~ContextPickerEvents() = default;

    std::atomic<ULONG> references_{1};
    std::optional<std::string> folder_path_;
};

void set_default_folder(IFileDialog* dialog, const std::string& path) {
    if (!dialog || path.empty()) return;
    const std::wstring wide_path = utf8_to_wide(path);
    if (wide_path.empty()) return;
    IShellItem* item = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(
            wide_path.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
        dialog->SetDefaultFolder(item);
        item->Release();
    }
}

} // namespace

ContextPickOutcome pick_context_items(void* parent_hwnd,
                                      const std::string& default_folder) {
    ContextPickOutcome outcome;

    const HRESULT init = ::CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(init);
    if (!uninitialize && init != RPC_E_CHANGED_MODE && init != S_FALSE) {
        outcome.error = "failed to initialize native context picker";
        return outcome;
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = ::CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        outcome.error = "failed to create native context picker";
        if (uninitialize) ::CoUninitialize();
        return outcome;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                           FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    }
    dialog->SetTitle(kContextPickerTitle);
    set_default_folder(dialog, default_folder);

    IFileDialogCustomize* customize = nullptr;
    bool folder_button_added = false;
    if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&customize))) && customize) {
        if (SUCCEEDED(customize->AddPushButton(
                kSelectCurrentFolderButton, kSelectCurrentFolderLabel))) {
            folder_button_added = true;
            customize->MakeProminent(kSelectCurrentFolderButton);
        }
        customize->Release();
    }
    if (!folder_button_added) {
        outcome.error = "failed to add folder action to native context picker";
        dialog->Release();
        if (uninitialize) ::CoUninitialize();
        return outcome;
    }

    auto* events = new ContextPickerEvents();
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(dialog->Advise(events, &cookie));
    if (!advised) {
        outcome.error = "failed to attach native context picker events";
        events->Release();
        dialog->Release();
        if (uninitialize) ::CoUninitialize();
        return outcome;
    }

    hr = dialog->Show(reinterpret_cast<HWND>(parent_hwnd));
    if (events->folder_path()) {
        outcome.folder_path = events->folder_path();
    } else if (SUCCEEDED(hr)) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count))) {
                outcome.file_paths.reserve(count);
                for (DWORD index = 0; index < count; ++index) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(items->GetItemAt(index, &item)) && item) {
                        if (!shell_item_is_folder(item)) {
                            if (auto path = filesystem_path(item)) {
                                outcome.file_paths.push_back(*path);
                            }
                        }
                        item->Release();
                    }
                }
            }
            items->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        outcome.error = "native context picker failed";
        LOG_WARN("[context_picker] IFileOpenDialog::Show failed");
    }

    dialog->Unadvise(cookie);
    events->Release();
    dialog->Release();
    if (uninitialize) ::CoUninitialize();
    return outcome;
}

} // namespace acecode::desktop

#else

namespace acecode::desktop {

ContextPickOutcome pick_context_items(void*, const std::string&) {
    ContextPickOutcome outcome;
    outcome.error = "unified native context picker is unavailable";
    return outcome;
}

} // namespace acecode::desktop

#endif
