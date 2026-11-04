// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include "native_desktop.h"

// Kept separate from Island's graphics, media and WinRT imports deliberately.
namespace {
std::wstring profile;
bool silent = false;
std::wstring quote(const std::wstring &value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
bool alive(DWORD pid) {
    if (!pid)
        return false;
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p)
        return GetLastError() == ERROR_ACCESS_DENIED;
    bool result = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return result;
}
void log(const std::wstring &stage, DWORD error = 0, DWORD pid = 0) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t line[512];
    swprintf(line, 512, L"%04u-%02u-%02u %02u:%02u:%02u %ls error=%lu pid=%lu\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond, stage.c_str(), error, pid);
    auto path = profile + L"\\startup.log";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 262144)
        MoveFileExW(path.c_str(), (profile + L"\\startup.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    char bytes[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, bytes, 2048, nullptr, nullptr);
    DWORD count;
    if (n > 1)
        WriteFile(f, bytes, n - 1, &count, nullptr);
    CloseHandle(f);
}
HWND nativeBar() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            wchar_t cls[64];
            GetClassNameW(w, cls, 64);
            if (!wcscmp(cls, L"Shell_TrayWnd") && !GetPropW(w, L"OwnIsland.TrayEndpoint")) {
                *(HWND *)p = w;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&found);
    return found;
}
bool owned(HWND w, const std::wstring &executable) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p)
        return false;
    wchar_t path[32768];
    DWORD n = 32768;
    bool same = QueryFullProcessImageNameW(p, 0, path, &n) && !_wcsicmp(path, executable.c_str());
    CloseHandle(p);
    return same;
}
void restoreOrphan() {
    auto file = profile + L"\\shell-restore.ini";
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\OwnIsland.ShellRestoreLock");
    if (!mutex)
        return;
    DWORD wait = WaitForSingleObject(mutex, 2500);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        DWORD pid = GetPrivateProfileIntW(L"shell", L"pid", 0, file.c_str());
        if (pid && !alive(pid) && (GetPrivateProfileIntW(L"shell",L"armed",0,file.c_str())||GetPrivateProfileIntW(L"native-desktop",L"count",0,file.c_str()))) {
            nativeDesktopRecover(file);
            if (GetPrivateProfileIntW(L"shell", L"armed", 0, file.c_str())) {
                if (GetPrivateProfileIntW(L"shell", L"had-taskbar", 1, file.c_str())) {
                    APPBARDATA data{sizeof(data)};
                    data.hWnd = nativeBar();
                    data.lParam = GetPrivateProfileIntW(L"shell", L"state", ABS_ALWAYSONTOP, file.c_str());
                    if (data.hWnd)
                        SHAppBarMessage(ABM_SETSTATE, &data);
                    BOOL show = GetPrivateProfileIntW(L"shell", L"visible", 1, file.c_str());
                    EnumWindows(
                        [](HWND w, LPARAM show) -> BOOL {
                            wchar_t cls[64];
                            GetClassNameW(w, cls, 64);
                            if ((!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) &&
                                !GetPropW(w, L"OwnIsland.TrayEndpoint"))
                                ShowWindow(w, show ? SW_SHOWNOACTIVATE : SW_HIDE);
                            return TRUE;
                        },
                        show);
                }
                WritePrivateProfileStringW(L"shell", L"armed", L"0", file.c_str());
            }
            log(L"restored-orphan", 0, pid);
        }
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
}
void fallback(DWORD error) {
    restoreOrphan();
    if (!nativeBar() &&
        !GetPrivateProfileIntW(L"shell", L"no-explorer-fallback", 0, (profile + L"\\shell-restore.ini").c_str())) {
        auto result = (INT_PTR)ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        log(L"windows-fallback", result > 32 ? 0 : (DWORD)result);
    }
    if (!silent) {
        wchar_t message[512];
        swprintf(message, 512, L"Не удалось запустить Island. Код ошибки: %lu.\nЖурнал: startup.log", error);
        MessageBoxW(nullptr, message, L"Запуск", MB_OK | MB_ICONERROR);
    }
}
} // namespace
// Native desktop recovery needs only the profile path from the application layer.
std::wstring stateDirectory() { return profile; }
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return ERROR_INVALID_PARAMETER;
    wchar_t module[32768];
    GetModuleFileNameW(nullptr, module, 32768);
    std::wstring folder = module;
    folder.resize(folder.find_last_of(L"\\/"));
    auto executable = folder + L"\\Island.exe";
    wchar_t local[32768];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    profile = std::wstring(local) + L"\\OwnIsland";
    DWORD timeout = 30000;
    bool recover = false, commandOnly = false, explicitMode = false;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; i++) {
        std::wstring value = argv[i];
        if (value == L"--silent")
            silent = true;
        else if (value == L"--recover")
            recover = true;
        else if (value == L"--startup-timeout" && i + 1 < argc)
            timeout = std::clamp((DWORD)_wtoi(argv[++i]), 1000ul, 60000ul);
        else if (value == L"--state-dir" && i + 1 < argc) {
            wchar_t absolute[32768];
            auto raw = argv[++i];
            DWORD n = GetFullPathNameW(raw, 32768, absolute, nullptr);
            if (!n || n >= 32768) {
                LocalFree(argv);
                return ERROR_INVALID_NAME;
            }
            profile = absolute;
            args.push_back(value);
            args.push_back(profile);
        } else {
            args.push_back(value);
            if (value == L"--standalone" || value == L"--integrated")
                explicitMode = true;
            if (value == L"--quit" || value == L"--collapse" || value == L"--notify")
                commandOnly = true;
        }
    }
    LocalFree(argv);
    SHCreateDirectoryExW(nullptr, profile.c_str(), nullptr);
    if (recover) {
        HWND window = FindWindowW(L"OwnIsland.Surface", nullptr);
        if (window && owned(window, executable)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            PostMessageW(window, WM_CLOSE, 0, 0);
            auto deadline = GetTickCount64() + 15000;
            while (alive(pid) && GetTickCount64() < deadline)
                Sleep(50);
            if (alive(pid)) {
                log(L"recovery-busy", ERROR_BUSY, pid);
                return ERROR_BUSY;
            }
        }
        restoreOrphan();
        if (!nativeBar() &&
            !GetPrivateProfileIntW(L"shell", L"no-explorer-fallback", 0, (profile + L"\\shell-restore.ini").c_str()))
            ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        log(L"recovery-complete");
        return 0;
    }
    if (!explicitMode && !commandOnly)
        args.push_back(GetPrivateProfileIntW(L"shell", L"standalone", 1, (profile + L"\\settings.ini").c_str())
                           ? L"--standalone"
                           : L"--integrated");
    std::wstring command = quote(executable);
    for (auto &arg : args)
        command += L" " + quote(arg);
    DWORD failure = ERROR_FILE_NOT_FOUND;
    for (int attempt = 0; attempt < 2; attempt++) {
        restoreOrphan();
        STARTUPINFOW start{sizeof(start)};
        start.dwFlags = STARTF_USESHOWWINDOW;
        start.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        auto invocation = command;
        if (!CreateProcessW(executable.c_str(), invocation.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                            nullptr, &start, &child)) {
            failure = GetLastError();
            log(L"create-failed", failure);
            break;
        }
        auto name = L"Local\\OwnIsland.StartReady." + std::to_wstring(child.dwProcessId);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (!ready) {
            failure = GetLastError();
            TerminateProcess(child.hProcess, failure);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            break;
        }
        ResumeThread(child.hThread);
        CloseHandle(child.hThread);
        log(L"starting", 0, child.dwProcessId);
        HANDLE handles[] = {ready, child.hProcess};
        DWORD outcome = WaitForMultipleObjects(2, handles, FALSE, commandOnly ? 15000 : timeout);
        bool done = outcome == WAIT_OBJECT_0;
        if (outcome == WAIT_OBJECT_0 + 1) {
            DWORD code = 1;
            GetExitCodeProcess(child.hProcess, &code);
            failure = code ? code : ERROR_PROCESS_ABORTED;
            HWND current = FindWindowW(L"OwnIsland.Surface", nullptr);
            DWORD pid = 0;
            GetWindowThreadProcessId(current, &pid);
            DWORD_PTR answer = 0;
            if (code == 0 &&
                (commandOnly || (current && pid != child.dwProcessId && owned(current, executable) &&
                                 SendMessageTimeoutW(current, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1500, &answer))))
                done = true;
        }
        if (outcome == WAIT_TIMEOUT) {
            failure = ERROR_TIMEOUT;
            if (!commandOnly) {
                HWND window=FindWindowW(L"OwnIsland.Surface",nullptr);DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(window&&pid==child.dwProcessId&&GetPropW(window,L"OwnIsland.StartupReady"))done=true;
                log(L"startup-timeout", failure, child.dwProcessId);
                if(!done)TerminateProcess(child.hProcess, failure);
                WaitForSingleObject(child.hProcess, 3000);
            }
        }
        bool guarded = false;
        auto guardName = L"Local\\OwnIsland.ShellReady." + std::to_wstring(child.dwProcessId);
        HANDLE guard = OpenEventW(SYNCHRONIZE, FALSE, guardName.c_str());
        if (guard) {
            guarded = true;
            CloseHandle(guard);
        }
        if (!guarded) {
            auto state = profile + L"\\shell-restore.ini";
            DWORD recorded = GetPrivateProfileIntW(L"shell", L"pid", 0, state.c_str());
            guarded = (recorded == child.dwProcessId || alive(recorded)) &&
                      !GetPrivateProfileIntW(L"shell", L"shutdown", 1, state.c_str());
        }
        CloseHandle(ready);
        CloseHandle(child.hProcess);
        if (done) {
            log(L"ready");
            return 0;
        }
        if (commandOnly) {
            log(L"command-failed", failure);
            return (int)failure;
        }
        if (guarded) {
            auto deadline = GetTickCount64() + 12000;
            while (GetTickCount64() < deadline) {
                HWND replacement = FindWindowW(L"OwnIsland.Surface", nullptr);
                if (replacement && owned(replacement, executable) && GetPropW(replacement, L"OwnIsland.StartupReady")) {
                    log(L"guard-recovered");
                    return 0;
                }
                Sleep(50);
            }
            break;
        }
    }
    log(L"failed", failure);
    fallback(failure);
    return (int)failure;
}
