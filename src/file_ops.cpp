// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "health.h"
#include "file_ops.h"
#include "graphics.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sherrors.h>

namespace files {
namespace fs = std::filesystem;
bool absolutePath(const std::wstring &p) {
    std::wstring path = p;
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        if (_wcsnicmp(path.c_str() + 4, L"UNC\\", 4) == 0)
            path = L"\\\\" + path.substr(8);
        else if (path.size() > 6 && path[5] == L':' && (path[6] == L'\\' || path[6] == L'/'))
            path = path.substr(4);
        else
            return false;
    }
    return !path.empty() && fs::path(path).is_absolute() && path.find_first_of(L"*?\0", 0, 3) == std::wstring::npos &&
           path.rfind(L"\\\\.\\", 0) != 0;
}
bool validName(const std::wstring &n) {
    if (n.empty() || n == L"." || n == L".." || n.size() > 255 || n.back() == L'.' || n.back() == L' ' ||
        n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return false;
    for (auto c : n)
        if (c < 32)
            return false;
    auto base = n.substr(0, n.find(L'.'));
    for (auto &c : base)
        c = towupper(c);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL")
        return false;
    if (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") && base[3] >= L'1' &&
        base[3] <= L'9')
        return false;
    return true;
}
std::wstring errorText(HRESULT hr) {
    wchar_t *buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   (DWORD)hr, 0, (wchar_t *)&buffer, 0, nullptr);
    std::wstring s = buffer ? buffer : L"Не удалось выполнить операцию";
    if (buffer)
        LocalFree(buffer);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    return s;
}
std::wstring quoteArgument(const std::wstring &s) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
        } else {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
namespace {
std::wstring itemPath(IShellItem *item) {
    PWSTR p = nullptr;
    if (!item)
        return {};
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &p);
    if (FAILED(hr))
        hr = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p);
    std::wstring s = SUCCEEDED(hr) && p ? p : L"";
    CoTaskMemFree(p);
    return s;
}
struct Sink final : IFileOperationProgressSink {
    LONG refs = 1;
    Operations *owner;
    Operation kind;
    std::set<std::wstring> freshCopies, freshMoves, freshRenames;
    Sink(Operations *o, Operation k) : owner(o), kind(k) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_IFileOperationProgressSink) {
            *p = static_cast<IFileOperationProgressSink *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = InterlockedDecrement(&refs);
        if (!n)
            delete this;
        return n;
    }
    HRESULT check(IShellItem *item = nullptr) {
        if (item)
            owner->update(0, 0, itemPath(item));
        return owner->cancelled() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
    }
    HRESULT record(IShellItem *source, IShellItem *result, HRESULT hr) {
        owner->itemStatus(hr);
        if (SUCCEEDED(hr) && hr != COPYENGINE_S_USER_IGNORED) {
            auto src = itemPath(source), dst = itemPath(result);
            owner->record({kind, src, dst,
                           ((kind == Operation::Move && freshMoves.count(src)) ||
                            (kind == Operation::Rename && freshRenames.count(src)) || kind == Operation::Recycle ||
                            kind == Operation::NewFolder || kind == Operation::NewText ||
                            (kind == Operation::Copy && freshCopies.count(src))) &&
                               !dst.empty()});
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return check(); }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *s, LPCWSTR name) override {
        auto source = itemPath(s);
        auto target = fs::path(source).parent_path() / name;
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
            freshRenames.insert(source);
        return check(s);
    }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *s, LPCWSTR, HRESULT hr, IShellItem *n) override {
        return record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *s, IShellItem *destination, LPCWSTR name) override {
        auto source = itemPath(s);
        auto target = fs::path(itemPath(destination)) / (name && *name ? name : fs::path(source).filename().c_str());
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
            freshMoves.insert(source);
        return check(s);
    }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *s, IShellItem *, LPCWSTR, HRESULT hr,
                                           IShellItem *n) override {
        return record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *s, IShellItem *destination, LPCWSTR name) override {
        auto source = itemPath(s), parent = itemPath(destination);
        auto target = fs::path(parent) / (name && *name ? name : fs::path(source).filename().c_str());
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
            freshCopies.insert(source);
        return check(s);
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *s, IShellItem *, LPCWSTR, HRESULT hr,
                                           IShellItem *n) override {
        return record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem *s) override { return check(s); }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *s, HRESULT hr, IShellItem *n) override {
        return record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return check(); }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT hr,
                                          IShellItem *n) override {
        return record(nullptr, n, hr);
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT total, UINT done) override {
        owner->update(total, done);
        return check();
    }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
std::wstring psQuote(const std::wstring &s) {
    std::wstring r = L"'";
    for (auto c : s) {
        r += c;
        if (c == L'\'')
            r += c;
    }
    return r + L"'";
}
std::wstring base64(const std::wstring &script) {
    static const wchar_t *table = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto bytes = (const unsigned char *)script.data();
    size_t n = script.size() * 2;
    std::wstring out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)bytes[i] << 16;
        if (i + 1 < n)
            v |= (unsigned)bytes[i + 1] << 8;
        if (i + 2 < n)
            v |= bytes[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < n ? table[(v >> 6) & 63] : L'=';
        out += i + 2 < n ? table[v & 63] : L'=';
    }
    return out;
}
HRESULT carryOrigin(const std::wstring &source, const std::wstring &destination, Operations &operation) {
    HANDLE input = CreateFileW((source + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME
                   ? S_OK
                   : HRESULT_FROM_WIN32(error);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input, &size) || size.QuadPart > 1048576) {
        CloseHandle(input);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    std::vector<BYTE> bytes((size_t)size.QuadPart);
    DWORD count = 0;
    bool read = ReadFile(input, bytes.data(), (DWORD)bytes.size(), &count, nullptr) && count == bytes.size();
    CloseHandle(input);
    if (!read)
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    std::error_code error;
    for (fs::recursive_directory_iterator it(destination, error), end; it != end; it.increment(error)) {
        if (error)
            return HRESULT_FROM_WIN32(error.value());
        if (operation.cancelled())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (!it->is_regular_file(error))
            continue;
        auto path = it->path().wstring() + L":Zone.Identifier";
        HANDLE output =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE)
            return HRESULT_FROM_WIN32(GetLastError());
        DWORD written = 0;
        bool saved = WriteFile(output, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
        CloseHandle(output);
        if (!saved)
            return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    return error ? HRESULT_FROM_WIN32(error.value()) : S_OK;
}
HRESULT archive(Operations &op, const Job &job) {
    // A private staging path is never merged into an existing directory. MoveFile
    // commits only the complete result; cancelling removes only our own staging.
    if (job.sources.empty() || !absolutePath(job.destination) || !validName(job.name))
        return E_INVALIDARG;
    for (auto &source : job.sources)
        if (!absolutePath(source) || source.find_first_of(L"\r\n") != std::wstring::npos)
            return E_INVALIDARG;
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t id[64] = {};
    StringFromGUID2(guid, id, 64);
    auto output = (fs::path(job.destination) / job.name).wstring();
    auto staging = output + L".island-" + id;
    if (GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (job.kind == Operation::Zip) {
        HANDLE f = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(f);
    } else if (!CreateDirectoryW(staging.c_str(), nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
    auto errorFile = staging + L".error", sourceFile = staging + L".sources";
    HANDLE manifest =
        CreateFileW(sourceFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (manifest == INVALID_HANDLE_VALUE) {
        std::error_code error;
        fs::remove_all(staging, error);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::wstring lines(1, 0xfeff);
    for (auto &source : job.sources)
        lines += source + L"\r\n";
    DWORD written = 0;
    BOOL saved = WriteFile(manifest, lines.data(), (DWORD)(lines.size() * 2), &written, nullptr);
    CloseHandle(manifest);
    if (!saved || written != lines.size() * 2) {
        DeleteFileW(sourceFile.c_str());
        std::error_code error;
        fs::remove_all(staging, error);
        return E_FAIL;
    }

    std::wstring script = L"$ErrorActionPreference='Stop';Add-Type -AssemblyName System.IO.Compression;Add-Type "
                          L"-AssemblyName System.IO.Compression.FileSystem;try {";
    if (job.kind == Operation::Zip) {
        script += L"$stream=[IO.File]::Open(" + psQuote(staging) +
                  L",[IO.FileMode]::Truncate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$z=[IO.Compression."
                  L"ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$false);try {function "
                  L"AddEntry($p,$n) {$a=[IO.File]::GetAttributes($p);if(($a-band [IO.FileAttributes]::ReparsePoint)-ne "
                  L"0){throw 'Ссылки и точки повторного анализа не включаются в архив'};if(($a-band "
                  L"[IO.FileAttributes]::Directory)-ne 0){[void]$z.CreateEntry($n+'/');foreach($c in "
                  L"[IO.Directory]::EnumerateFileSystemEntries($p)){AddEntry $c "
                  L"($n+'/"
                  L"'+[IO.Path]::GetFileName($c))}}else{[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($"
                  L"z,$p,$n,[IO.Compression.CompressionLevel]::Optimal)}};";
        script += L"foreach($src in [IO.File]::ReadAllLines(" + psQuote(sourceFile) +
                  L")){AddEntry $src ([IO.Path]::GetFileName($src))};";
        script += L"}finally{$z.Dispose()};";
    } else {
        script +=
            L"$root=[IO.Path]::GetFullPath(" + psQuote(staging) + L")+'\\';$z=[IO.Compression.ZipFile]::OpenRead(" +
            psQuote(job.sources[0]) +
            L");try {foreach($e in $z.Entries){$n=$e.FullName.Replace('/','\\');if($n.Contains(':')-or "
            L"$n.StartsWith('\\')-or (($e.ExternalAttributes-shr 16)-band 0xf000)-eq 0xa000){throw 'Недопустимый путь "
            L"в архиве'};$parts=$n.TrimEnd('\\').Split('\\');foreach($part in "
            L"$parts){if([string]::IsNullOrEmpty($part)-or $part-eq '.'-or $part-eq '..'-or $part.EndsWith('.')-or "
            L"$part.EndsWith(' ')-or $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars())-ge 0-or $part-match "
            L"'^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\.|$)'){throw 'Недопустимое имя в "
            L"архиве'}};$p=[IO.Path]::GetFullPath([IO.Path]::Combine($root,$n));if(-not "
            L"$p.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Путь в архиве выходит за "
            L"папку'};if($n.EndsWith('\\')){[void][IO.Directory]::CreateDirectory($p)}else{[void][IO.Directory]::"
            L"CreateDirectory([IO.Path]::GetDirectoryName($p));[IO.Compression.ZipFileExtensions]::ExtractToFile($e,$p,"
            L"$false)}}}finally{$z.Dispose()};";
    }
    script += L"exit 0}catch{[IO.File]::WriteAllText(" + psQuote(errorFile) + L",$_.Exception.Message);exit 1}";
    wchar_t sys[32768] = {};
    GetSystemDirectoryW(sys, 32768);
    auto exe = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command =
        quoteArgument(exe) + L" -NoLogo -NoProfile -NonInteractive -EncodedCommand " + base64(script);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    HANDLE processJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (processJob)
        SetInformationJobObject(processJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    HRESULT hr = E_FAIL;
    if (command.size() < 32700 && processJob &&
        CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                       nullptr, nullptr, &si, &pi)) {
        if (AssignProcessToJobObject(processJob, pi.hProcess)) {
            ResumeThread(pi.hThread);
            while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
                if (op.cancelled()) {
                    TerminateJobObject(processJob, ERROR_CANCELLED);
                    break;
                }
            }
            WaitForSingleObject(pi.hProcess, 5000);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            hr = op.cancelled() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : code == 0 ? S_OK : E_FAIL;
        } else
            TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else
        hr = HRESULT_FROM_WIN32(command.size() >= 32700 ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
    if (processJob)
        CloseHandle(processJob);
    if (SUCCEEDED(hr) && job.kind == Operation::Extract)
        hr = carryOrigin(job.sources[0], staging, op);
    if (SUCCEEDED(hr) && !MoveFileW(staging.c_str(), output.c_str()))
        hr = HRESULT_FROM_WIN32(GetLastError());
    std::error_code ec;
    if (FAILED(hr))
        fs::remove_all(staging, ec);
    if (FAILED(hr) && !op.cancelled()) {
        std::ifstream input(fs::path(errorFile), std::ios::binary);
        std::string message((std::istreambuf_iterator<char>(input)), {});
        if (message.size() > 4096)
            message.resize(4096);
        int n = MultiByteToWideChar(CP_UTF8, 0, message.data(), (int)message.size(), nullptr, 0);
        if (n) {
            std::wstring wide(n, 0);
            MultiByteToWideChar(CP_UTF8, 0, message.data(), (int)message.size(), wide.data(), n);
            op.update(0, 0, L"ZIP: " + wide);
        }
    }
    DeleteFileW(errorFile.c_str());
    DeleteFileW(sourceFile.c_str());
    if (SUCCEEDED(hr))
        op.record({job.kind, L"", output, true});
    return hr;
}
} // namespace
namespace {
std::wstring nativePath(const std::wstring &path) {
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    auto p = fs::path(path).lexically_normal().wstring();
    return p.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + p.substr(2) : L"\\\\?\\" + p;
}
bool longJob(const Job &job) {
    if (job.destination.size() >= 248)
        return true;
    for (auto &p : job.sources)
        if (p.size() >= 248 || p.rfind(L"\\\\?\\", 0) == 0)
            return true;
    for (auto &p : job.targets)
        if (p.size() >= 248 || p.rfind(L"\\\\?\\", 0) == 0)
            return true;
    return false;
}
struct CopyProgress {
    Operations *operations;
    unsigned index, total;
};
DWORD CALLBACK copyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE,
                            HANDLE, LPVOID raw) {
    auto &state = *(CopyProgress *)raw;
    state.operations->update(state.total * 1000,
                             state.index * 1000 +
                                 (unsigned)(total.QuadPart ? done.QuadPart * 1000 / total.QuadPart : 0));
    return state.operations->cancelled() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
void removeStaging(const std::wstring &path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    DeleteFileW(path.c_str());
}
HRESULT longFileJob(Operations &operations, const Job &job) {
    // Shell parsing still rejects some extended paths on Windows 10. This path
    // handles literal filesystem items, preserves streams via CopyFileEx, and
    // commits to a previously absent name. It never retries a partial shell job.
    if (job.kind == Operation::Recycle || job.kind == Operation::Link)
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    if (job.kind == Operation::NewFolder || job.kind == Operation::NewText) {
        if (!absolutePath(job.destination) || !validName(job.name))
            return E_INVALIDARG;
        auto target = (fs::path(job.destination) / job.name).wstring();
        auto native = nativePath(target);
        bool created = false;
        if (job.kind == Operation::NewFolder)
            created = CreateDirectoryW(native.c_str(), nullptr) != FALSE;
        else {
            HANDLE file =
                CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            created = file != INVALID_HANDLE_VALUE;
            if (created)
                CloseHandle(file);
        }
        if (!created)
            return HRESULT_FROM_WIN32(GetLastError());
        operations.record({job.kind, L"", target, true});
        return S_OK;
    }
    struct Plan {
        std::wstring source, target;
        DWORD attributes;
    };
    std::vector<Plan> plan;
    for (size_t i = 0; i < job.sources.size(); i++) {
        auto &source = job.sources[i];
        if (!absolutePath(source))
            return E_INVALIDARG;
        DWORD attributes = GetFileAttributesW(nativePath(source).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(GetLastError());
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && (job.kind == Operation::Copy || job.kind == Operation::Delete))
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        std::wstring target;
        if (job.kind != Operation::Delete) {
            if (!job.targets.empty()) {
                if (i >= job.targets.size())
                    return E_INVALIDARG;
                target = job.targets[i];
            } else if (job.kind == Operation::Rename) {
                if (!validName(job.name))
                    return E_INVALIDARG;
                target = (fs::path(source).parent_path() / job.name).wstring();
            } else {
                if (!absolutePath(job.destination))
                    return E_INVALIDARG;
                target =
                    (fs::path(job.destination) / (job.name.empty() ? fs::path(source).filename().wstring() : job.name))
                        .wstring();
            }
            if (!absolutePath(target) || !validName(fs::path(target).filename().wstring()) ||
                !_wcsicmp(source.c_str(), target.c_str()))
                return E_INVALIDARG;
            if (GetFileAttributesW(nativePath(target).c_str()) != INVALID_FILE_ATTRIBUTES)
                return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        }
        plan.push_back({source, target, attributes});
    }
    for (unsigned i = 0; i < plan.size(); i++) {
        auto &item = plan[i];
        if (operations.cancelled())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        auto source = nativePath(item.source), target = nativePath(item.target);
        operations.update((unsigned)plan.size() * 1000, i * 1000, item.source);
        if (job.kind == Operation::Delete) {
            if (!DeleteFileW(source.c_str()))
                return HRESULT_FROM_WIN32(GetLastError());
            operations.record({job.kind, item.source, L"", false});
            continue;
        }
        if (job.kind == Operation::Move || job.kind == Operation::Rename) {
            if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                operations.record({job.kind, item.source, item.target, true});
                continue;
            }
            DWORD error = GetLastError();
            if (error != ERROR_NOT_SAME_DEVICE || (item.attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                job.kind == Operation::Rename)
                return HRESULT_FROM_WIN32(error);
        }
        GUID guid{};
        CoCreateGuid(&guid);
        wchar_t value[64];
        StringFromGUID2(guid, value, 64);
        auto staging = nativePath(
            (fs::path(item.target).parent_path() / (L".island-transfer-" + std::wstring(value) + L".tmp")).wstring());
        HANDLE reservation =
            CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation == INVALID_HANDLE_VALUE)
            return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(reservation);
        CopyProgress state{&operations, i, (unsigned)plan.size()};
        if (!CopyFileExW(source.c_str(), staging.c_str(), copyProgress, &state, nullptr, 0)) {
            DWORD error = GetLastError();
            removeStaging(staging);
            return HRESULT_FROM_WIN32(error);
        }
        if (operations.cancelled()) {
            removeStaging(staging);
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            DWORD error = GetLastError();
            removeStaging(staging);
            return HRESULT_FROM_WIN32(error);
        }
        if (job.kind == Operation::Move) {
            if (!DeleteFileW(source.c_str())) {
                DWORD error = GetLastError();
                operations.record({Operation::Copy, item.source, item.target, true});
                return HRESULT_FROM_WIN32(error);
            }
            operations.record({Operation::Move, item.source, item.target, true});
        } else
            operations.record({Operation::Copy, item.source, item.target, true});
    }
    return S_OK;
}
} // namespace
Operations::~Operations() {
    cancel();
    if (thread.joinable())
        thread.join();
}
Progress Operations::snapshot(bool includeChanges) const {
    std::lock_guard lock(mutex);
    Progress value;
    value.busy = current.busy;
    value.cancelled = current.cancelled;
    value.error = current.error;
    value.done = current.done;
    value.total = current.total;
    value.failed = current.failed;
    value.skipped = current.skipped;
    value.name = current.name;
    if (includeChanges)
        value.changes = current.changes;
    return value;
}
void Operations::itemStatus(HRESULT hr) {
    std::lock_guard lock(mutex);
    if (hr == COPYENGINE_S_USER_IGNORED) {
        ++current.skipped;
        return;
    }
    if (hr == COPYENGINE_E_USER_CANCELLED || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT) {
        current.cancelled = true;
        return;
    }
    if (FAILED(hr)) {
        ++current.failed;
        if (SUCCEEDED(current.error))
            current.error = hr;
    }
}
void Operations::update(unsigned total, unsigned done, const std::wstring &name) {
    std::lock_guard lock(mutex);
    if (total) {
        current.total = total;
        current.done = done;
    }
    if (!name.empty())
        current.name = name;
}
void Operations::record(Change c) {
    std::lock_guard lock(mutex);
    current.changes.push_back(std::move(c));
}
bool Operations::start(HWND owner, Job job) {
    if (snapshot(false).busy)
        return false;
    if (thread.joinable())
        thread.join();
    cancelling = false;
    {
        std::lock_guard lock(mutex);
        current = {};
        current.busy = true;
        current.total = (unsigned)job.sources.size();
    }
    auto protection=std::make_shared<health::Hold>();
    if(!*protection){std::lock_guard lock(mutex);current.busy=false;current.error=HRESULT_FROM_WIN32(ERROR_BUSY);return false;}
    thread = std::thread([this, owner, job = std::move(job), protection] {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), hr = init;
        BOOL aborted = FALSE;
        if (SUCCEEDED(init)) {
            if (job.kind == Operation::Zip || job.kind == Operation::Extract)
                hr = archive(*this, job);
            else if (longJob(job))
                hr = longFileJob(*this, job);
            else {
                Com<IFileOperation> operation;
                hr =
                    CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(operation.put()));
                if (SUCCEEDED(hr)) {
                    hr = operation->SetOwnerWindow(owner);
                    DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT | FOFX_EARLYFAILURE;
                    if (job.kind == Operation::Recycle)
                        flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;
                    if (job.quiet)
                        flags |= FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
                    if (SUCCEEDED(hr))
                        hr = operation->SetOperationFlags(flags);
                    auto sink = new Sink(this, job.kind);
                    DWORD cookie = 0;
                    if (SUCCEEDED(hr))
                        hr = operation->Advise(sink, &cookie);
                    sink->Release();
                    Com<IShellItem> destination;
                    if (SUCCEEDED(hr) && !job.destination.empty())
                        hr = SHCreateItemFromParsingName(job.destination.c_str(), nullptr,
                                                         IID_PPV_ARGS(destination.put()));
                    if (SUCCEEDED(hr) && (job.kind == Operation::NewFolder || job.kind == Operation::NewText)) {
                        if (!destination || !validName(job.name))
                            hr = E_INVALIDARG;
                        else
                            hr = operation->NewItem(destination.p,
                                                    job.kind == Operation::NewFolder ? FILE_ATTRIBUTE_DIRECTORY
                                                                                     : FILE_ATTRIBUTE_NORMAL,
                                                    job.name.c_str(), nullptr, nullptr);
                    }
                    for (size_t index = 0; index < job.sources.size(); index++) {
                        auto &path = job.sources[index];
                        if (FAILED(hr))
                            break;
                        Com<IShellItem> item;
                        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()));
                        if (FAILED(hr))
                            break;
                        std::wstring targetName = job.name;
                        if (!job.targets.empty()) {
                            if (index >= job.targets.size()) {
                                hr = E_INVALIDARG;
                                break;
                            }
                            auto target = fs::path(job.targets[index]);
                            targetName = target.filename().wstring();
                            hr = SHCreateItemFromParsingName(target.parent_path().c_str(), nullptr,
                                                             IID_PPV_ARGS(destination.put()));
                            if (FAILED(hr))
                                break;
                        }
                        LPCWSTR name = targetName.empty() ? nullptr : targetName.c_str();
                        switch (job.kind) {
                        case Operation::Copy:
                            hr = operation->CopyItem(item.p, destination.p, name, nullptr);
                            break;
                        case Operation::Move:
                            hr = operation->MoveItem(item.p, destination.p, name, nullptr);
                            break;
                        case Operation::Rename:
                            hr = validName(targetName) ? operation->RenameItem(item.p, targetName.c_str(), nullptr)
                                                       : E_INVALIDARG;
                            break;
                        case Operation::Recycle:
                        case Operation::Delete:
                            hr = operation->DeleteItem(item.p, nullptr);
                            break;
                        case Operation::Link: {
                            Com<IShellLinkW> link;
                            hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(link.put()));
                            if (SUCCEEDED(hr)) {
                                link->SetPath(path.c_str());
                                Com<IPersistFile> persist;
                                hr = link->QueryInterface(IID_PPV_ARGS(persist.put()));
                                auto target = (fs::path(job.destination) /
                                               (fs::path(path).filename().wstring() + L" — ярлык.lnk"))
                                                  .wstring();
                                if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
                                    hr = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
                                if (SUCCEEDED(hr))
                                    hr = persist->Save(target.c_str(), TRUE);
                                if (SUCCEEDED(hr))
                                    record({Operation::Link, path, target, true});
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    }
                    if (SUCCEEDED(hr) && job.kind != Operation::Link)
                        hr = operation->PerformOperations();
                    operation->GetAnyOperationsAborted(&aborted);
                    if (cookie)
                        operation->Unadvise(cookie);
                }
            }
            CoUninitialize();
        }
        {
            std::lock_guard lock(mutex);
            current.busy = false;
            current.cancelled = current.cancelled || aborted || cancelling ||
                                hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED;
            if (FAILED(hr) || SUCCEEDED(current.error))
                current.error = hr;
        }
        PostMessageW(owner, WM_APP + 3, 0, 0);
    });
    return true;
}
std::vector<std::wstring> dropPaths(IDataObject *data) {
    std::vector<std::wstring> paths;
    if (!data)
        return paths;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&format, &medium))) {
        auto drop = (HDROP)medium.hGlobal;
        UINT n = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        for (UINT i = 0; i < n && i < 10000; i++) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length > 32767)
                continue;
            std::wstring p(length + 1, 0);
            DragQueryFileW(drop, i, p.data(), length + 1);
            p.resize(length);
            if (absolutePath(p))
                paths.push_back(std::move(p));
        }
        ReleaseStgMedium(&medium);
    }
    if (!paths.empty())
        return paths;
    format.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    medium = {};
    if (FAILED(data->GetData(&format, &medium)))
        return paths;
    SIZE_T size = GlobalSize(medium.hGlobal);
    auto bytes = (const BYTE *)GlobalLock(medium.hGlobal);
    if (bytes && size >= 8) {
        UINT n = 0;
        memcpy(&n, bytes, 4);
        if (n <= 10000 && size >= 4 + (SIZE_T)(n + 1) * 4) {
            auto idAt = [&](UINT i) -> PCUIDLIST_RELATIVE {
                UINT offset = 0;
                memcpy(&offset, bytes + 4 + i * 4, 4);
                if (offset < 4 + (n + 1) * 4 || offset >= size)
                    return nullptr;
                SIZE_T p = offset;
                while (p + 2 <= size) {
                    USHORT length = 0;
                    memcpy(&length, bytes + p, 2);
                    if (length == 0)
                        return (PCUIDLIST_RELATIVE)(bytes + offset);
                    if (length < 2 || length > size - p)
                        return nullptr;
                    p += length;
                }
                return nullptr;
            };
            auto parent = idAt(0);
            if (parent)
                for (UINT i = 1; i <= n; i++) {
                    auto child = idAt(i);
                    if (!child)
                        continue;
                    auto full = ILCombine(parent, child);
                    PWSTR name = nullptr;
                    if (full && SUCCEEDED(SHGetNameFromIDList(full, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name)
                        paths.emplace_back(name);
                    CoTaskMemFree(name);
                    CoTaskMemFree(full);
                }
        }
    }
    if (bytes)
        GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return paths;
}
HRESULT dataObject(const std::vector<std::wstring> &paths, IDataObject **result) {
    if (!result)
        return E_POINTER;
    *result = nullptr;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    HRESULT hr = S_OK;
    for (auto &p : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        hr = SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr);
        if (FAILED(hr))
            break;
        pidls.push_back(id);
    }
    if (SUCCEEDED(hr) && !pidls.empty()) {
        Com<IShellItemArray> array;
        hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), array.put());
        if (SUCCEEDED(hr))
            hr = array->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject, (void **)result);
    } else if (SUCCEEDED(hr))
        hr = E_INVALIDARG;
    for (auto p : pidls)
        CoTaskMemFree(p);
    return hr;
}
bool clipboard(HWND owner, const std::vector<std::wstring> &paths, bool cut) {
    if (paths.empty())
        return false;
    bool virtualItem = false;
    for (auto &p : paths)
        virtualItem |= GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (virtualItem) {
        Com<IDataObject> data;
        if (FAILED(dataObject(paths, data.put())))
            return false;
        FORMATETC format{(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), nullptr, DVASPECT_CONTENT, -1,
                         TYMED_HGLOBAL};
        STGMEDIUM value{};
        value.tymed = TYMED_HGLOBAL;
        value.hGlobal = GlobalAlloc(GHND, 4);
        if (!value.hGlobal)
            return false;
        auto pointer = (DWORD *)GlobalLock(value.hGlobal);
        if (!pointer) {
            GlobalFree(value.hGlobal);
            return false;
        }
        *pointer = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        GlobalUnlock(value.hGlobal);
        HRESULT hr = data->SetData(&format, &value, TRUE);
        if (FAILED(hr))
            ReleaseStgMedium(&value);
        if (FAILED(hr) || FAILED(OleSetClipboard(data.p)))
            return false;
        return SUCCEEDED(OleFlushClipboard());
    }
    if (!OpenClipboard(owner))
        return false;
    size_t chars = 1;
    for (auto &p : paths)
        chars += p.size() + 1;
    HGLOBAL memory = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * 2), effect = GlobalAlloc(GHND, 4);
    bool ok = false;
    if (memory && effect) {
        auto data = (DROPFILES *)GlobalLock(memory);
        auto e = (DWORD *)GlobalLock(effect);
        if (data && e) {
            data->pFiles = sizeof(DROPFILES);
            data->fWide = TRUE;
            auto dst = (wchar_t *)((BYTE *)data + sizeof(DROPFILES));
            for (auto &p : paths) {
                memcpy(dst, p.c_str(), (p.size() + 1) * 2);
                dst += p.size() + 1;
            }
            *e = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(memory);
            GlobalUnlock(effect);
            EmptyClipboard();
            if (SetClipboardData(CF_HDROP, memory)) {
                memory = nullptr;
                ok = true;
            }
            if (SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect))
                effect = nullptr;
        } else {
            if (data)
                GlobalUnlock(memory);
            if (e)
                GlobalUnlock(effect);
        }
    }
    if (memory)
        GlobalFree(memory);
    if (effect)
        GlobalFree(effect);
    CloseClipboard();
    return ok;
}
std::vector<std::wstring> clipboardPaths(HWND, DWORD &effect) {
    Com<IDataObject> data;
    effect = DROPEFFECT_COPY;
    std::vector<std::wstring> paths;
    if (SUCCEEDED(OleGetClipboard(data.put()))) {
        paths = dropPaths(data.p);
        FORMATETC f{(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), nullptr, DVASPECT_CONTENT, -1,
                    TYMED_HGLOBAL};
        STGMEDIUM m{};
        if (SUCCEEDED(data->GetData(&f, &m))) {
            if (GlobalSize(m.hGlobal) >= 4) {
                auto p = (DWORD *)GlobalLock(m.hGlobal);
                if (p) {
                    effect = *p;
                    GlobalUnlock(m.hGlobal);
                }
            }
            ReleaseStgMedium(&m);
        }
    }
    return paths;
}
void properties(HWND owner, const std::vector<std::wstring> &paths) {
        health::Hold protection;if(!protection)return;
    if (paths.empty())
        return;
    Com<IDataObject> data;
    if (SUCCEEDED(dataObject(paths, data.put())))
        SHMultiFileProperties(data.p, 0);
    else
        SHObjectProperties(owner, SHOP_FILEPATH, paths[0].c_str(), nullptr);
}
void openWith(HWND owner, const std::wstring &path) {
        health::Hold protection;if(!protection)return;
    OPENASINFO info{path.c_str(), nullptr, OAIF_EXEC};
    SHOpenWithDialog(owner, &info);
}


} // namespace files
