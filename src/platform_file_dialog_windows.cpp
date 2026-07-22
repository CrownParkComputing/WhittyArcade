#include "platform_file_dialog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace {
std::wstring utf8_to_wide(const char* text) {
    if (!text || !*text) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
                                         -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                        result.data(), size);
    result.pop_back();
    return result;
}

std::string wide_to_utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                         -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                        result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

void append_shell_item(IShellItem* item, std::vector<std::string>& paths) {
    if (!item) return;
    PWSTR native_path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &native_path))) {
        std::string path = wide_to_utf8(native_path);
        CoTaskMemFree(native_path);
        if (!path.empty()) paths.push_back(std::move(path));
    }
}
} // namespace

std::vector<std::string> platform_file_selection(bool directory,
                                                 bool multiple,
                                                 const char* title) {
    std::vector<std::string> paths;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return paths;

    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        if (uninitialize) CoUninitialize();
        return paths;
    }

    FILEOPENDIALOGOPTIONS options{};
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (directory) options |= FOS_PICKFOLDERS;
        if (multiple) options |= FOS_ALLOWMULTISELECT;
        result = dialog->SetOptions(options);
    }
    const std::wstring wide_title = utf8_to_wide(title);
    if (SUCCEEDED(result) && !wide_title.empty())
        result = dialog->SetTitle(wide_title.c_str());
    if (SUCCEEDED(result) && !directory && multiple) {
        const COMDLG_FILTERSPEC filters[]{
            {L"MAME ROM archives (*.zip)", L"*.zip"},
            {L"All files", L"*.*"},
        };
        result = dialog->SetFileTypes(2, filters);
        if (SUCCEEDED(result)) dialog->SetFileTypeIndex(1);
    }

    if (SUCCEEDED(result)) result = dialog->Show(nullptr);
    if (SUCCEEDED(result) && multiple && !directory) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count))) {
                for (DWORD index = 0; index < count; ++index) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(items->GetItemAt(index, &item))) {
                        append_shell_item(item, paths);
                        item->Release();
                    }
                }
            }
            items->Release();
        }
    } else if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            append_shell_item(item, paths);
            item->Release();
        }
    }

    dialog->Release();
    if (uninitialize) CoUninitialize();
    return paths;
}
