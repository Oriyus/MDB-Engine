#include "Platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <filesystem>

namespace Platform {

    namespace {
        std::wstring Native(const std::string& s) {
            return std::filesystem::u8path(s).make_preferred().wstring();   // backslashes for the shell
        }
    }

    bool OpenWithSystem(const std::string& path) {
        const auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", Native(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return r > 32;
    }

    void ShowInExplorer(const std::string& path) {
        const std::wstring native = Native(path);
        if (std::filesystem::is_directory(std::filesystem::u8path(path)))
            ShellExecuteW(nullptr, L"explore", native.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + native + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }

    bool MoveToRecycleBin(const std::string& path) {
        std::wstring from = Native(path);
        from.push_back(L'\0');                          // the list ends with a double NUL
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    }

} // namespace Platform
