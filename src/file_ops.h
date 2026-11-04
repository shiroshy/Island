// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <memory>

namespace files {
enum class Operation { Copy, Move, Recycle, Delete, Rename, NewFolder, NewText, Link, Zip, Extract };
struct Change {
    Operation kind;
    std::wstring source, result;
    bool undoable = false;
};
struct Job {
    Operation kind = Operation::Copy;
    std::vector<std::wstring> sources;
    std::wstring destination, name;
    bool quiet = false;
    std::vector<std::wstring> targets;
};
struct Progress {
    bool busy = false, cancelled = false;
    HRESULT error = S_OK;
    unsigned done = 0, total = 0, failed = 0, skipped = 0;
    std::wstring name;
    std::vector<Change> changes;
};
class Operations {
    std::thread thread;
    mutable std::mutex mutex;
    Progress current;
    std::atomic<bool> cancelling = false;

  public:
    ~Operations();
    bool start(HWND owner, Job job);
    void cancel() { cancelling = true; }
    bool cancelled() const { return cancelling; }
    Progress snapshot(bool includeChanges=true) const;
    void itemStatus(HRESULT error);
    void update(unsigned total, unsigned done, const std::wstring &name = L"");
    void record(Change change);
};
bool absolutePath(const std::wstring &path);
bool validName(const std::wstring &name);
std::wstring errorText(HRESULT error);
std::wstring quoteArgument(const std::wstring &value);
std::vector<std::wstring> dropPaths(IDataObject *data);
HRESULT dataObject(const std::vector<std::wstring> &paths, IDataObject **result);
bool clipboard(HWND owner, const std::vector<std::wstring> &paths, bool cut);
std::vector<std::wstring> clipboardPaths(HWND owner, DWORD &effect);
void properties(HWND owner, const std::vector<std::wstring> &paths);
void openWith(HWND owner, const std::wstring &path);

} // namespace files
