#include "Platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <filesystem>
#include <iterator>
#include <thread>

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

    namespace {
        std::string Utf8(const std::wstring& w) {
            if (w.empty()) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
            return s;
        }
        std::wstring Wide(const std::string& s) {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
            return w;
        }
        // One argument for a Windows command line (CommandLineToArgvW rules: quotes, and backslashes before them)
        std::wstring Quote(const std::wstring& a) {
            if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
            std::wstring out = L"\"";
            size_t slashes = 0;
            for (wchar_t c : a) {
                if (c == L'\\') { slashes++; continue; }
                if (c == L'"') out.append(slashes * 2 + 1, L'\\');
                else out.append(slashes, L'\\');
                slashes = 0;
                out += c;
            }
            out.append(slashes * 2, L'\\');
            return out + L"\"";
        }
    }

    // The shell dialog needs a single-threaded COM apartment, but the main thread is already multithreaded
    // (miniaudio initialises COM that way for WASAPI), and there the dialog hangs. So it runs on a thread of
    // its own, owned by the editor window, while the main thread waits (the dialog is modal anyway).
    static std::string PickFolderOnThisThread(const std::string& title, const std::string& startIn, HWND owner) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::string result;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(Wide(title).c_str());
            if (!startIn.empty()) {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(Native(startIn).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
            if (SUCCEEDED(dialog->Show(owner))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        const auto u8 = std::filesystem::path(path).generic_u8string();
                        result.assign(u8.begin(), u8.end());
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
        return result;
    }

    std::string PickFolder(const std::string& title, const std::string& startIn) {
        HWND owner = GetActiveWindow();
        std::string result;
        std::thread picker([&] { result = PickFolderOnThisThread(title, startIn, owner); });
        // Keep handling the editor window's messages while waiting: the dialog disables its owner and
        // activates it again with messages sent to this thread, so a plain join() would deadlock.
        const HANDLE done = picker.native_handle();
        while (MsgWaitForMultipleObjects(1, &done, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        picker.join();
        return result;
    }

    std::string ExeDir() {
        wchar_t exe[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
        const std::filesystem::path dir = (n == 0 || n >= std::size(exe)) ? std::filesystem::current_path()
                                                                          : std::filesystem::path(exe).parent_path();
        const std::u8string s = dir.generic_u8string();
        return std::string(s.begin(), s.end());
    }

    bool LaunchSelf(const std::vector<std::string>& args) {
        wchar_t exe[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
        if (n == 0 || n >= std::size(exe)) return false;
        std::wstring cmd = Quote(exe);
        for (const auto& a : args) cmd += L" " + Quote(Wide(a));
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        wchar_t cwd[MAX_PATH * 4];
        GetCurrentDirectoryW((DWORD)std::size(cwd), cwd);
        if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd, &si, &pi)) return false;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    int RunProcess(const std::string& commandLine, const std::function<void(const std::string&)>& onLine) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return -1;
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);     // only the child's end is inherited
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = Wide(commandLine);
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(writePipe);                                     // so ReadFile ends when the child does
        if (!ok) { CloseHandle(readPipe); return -1; }
        std::string pending;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
            pending.append(buffer, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                onLine(line);
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) onLine(pending);
        CloseHandle(readPipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return (int)code;
    }

} // namespace Platform
