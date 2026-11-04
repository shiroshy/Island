// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "health.h"
#include "file_browser.h"
#include "components.h"
#include "file_ops.h"
#include "search.h"
#include "platform.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <set>
#include <fstream>
#include <deque>
#include <array>
#include <functional>

namespace {
namespace fs = std::filesystem;
constexpr wchar_t Class[] = L"OwnIsland.Files";
MenuColors palette{0x09090b, 0x252328, 0xeae9ed, 0x8a8790, 0xb8d8c9};
std::atomic<uint64_t> nextImage = 0xd000000000000000ull;
struct Entry {
    std::wstring name, path, type;
    bool folder = false;
    uintmax_t bytes = 0;
    FILETIME modified{};
    DWORD attributes = 0;
    std::shared_ptr<const PixelImage> icon;
    std::vector<BYTE> shellId;
};
struct Listing {
    std::wstring path, error;
    std::vector<Entry> entries;
    uint64_t revision = 0;
    bool truncated = false, search = false;
};
std::vector<HWND> windows;
std::vector<std::vector<files::Change>> undoHistory, redoHistory;
std::wstring known(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        p = raw;
        CoTaskMemFree(raw);
    }
    return p;
}
std::wstring home() { return known(FOLDERID_Profile); }
std::wstring config() { return stateDirectory() + L"\\files.ini"; }
std::wstring text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    std::wstring s(n + 1, 0);
    GetWindowTextW(edit, s.data(), n + 1);
    s.resize(n);
    return s;
}
std::wstring lower(std::wstring s) {
    if (s.empty())
        return s;
    std::wstring out(s.size(), 0);
    return LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(), out.data(), (int)out.size(),
                         nullptr, nullptr, 0)
               ? out
               : s;
}
std::wstring sizeText(uintmax_t n) {
    wchar_t b[48];
    if (n < 1024)
        swprintf(b, 48, L"%llu Б", (unsigned long long)n);
    else if (n < 1048576)
        swprintf(b, 48, L"%.1f КБ", n / 1024.);
    else if (n < 1073741824)
        swprintf(b, 48, L"%.1f МБ", n / 1048576.);
    else
        swprintf(b, 48, L"%.1f ГБ", n / 1073741824.);
    return b;
}
std::wstring dateText(FILETIME utc) {
    FILETIME local{};
    SYSTEMTIME t{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &t))
        return {};
    wchar_t b[48];
    swprintf(b, 48, L"%02u.%02u.%04u %02u:%02u", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
    return b;
}
std::wstring label(const std::wstring &p) {
    if (p == L"::computer")
        return L"Этот компьютер";
    if (p == L"::recycle")
        return L"Корзина";
    if (p == L"::network")
        return L"Сеть";
    auto s = fs::path(p).filename().wstring();
    return s.empty() ? p : s;
}
bool shellPath(const std::wstring &p) {
    auto value = lower(p);
    return value.rfind(L"\\\\", 0) == 0 || value.rfind(L"::{", 0) == 0 || value.rfind(L"shell:", 0) == 0 ||
           value.ends_with(L".zip") || value.find(L".zip\\") != std::wstring::npos;
}
bool bindFolder(const std::wstring &p, IShellFolder **out) {
    Com<IShellItem> item;
    return SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(item.put()))) &&
           SUCCEEDED(item->BindToHandler(nullptr, BHID_SFObject, IID_IShellFolder, (void **)out));
}
std::shared_ptr<const PixelImage> bitmapPixels(HBITMAP bitmap) {
    BITMAP b{};
    if (!GetObjectW(bitmap, sizeof(b), &b) || b.bmWidth < 1 || b.bmHeight < 1 || b.bmWidth > 2048 || b.bmHeight > 2048)
        return {};
    auto out = std::make_shared<PixelImage>();
    out->width = b.bmWidth;
    out->height = b.bmHeight;
    out->id = ++nextImage;
    out->bgra.resize((size_t)b.bmWidth * b.bmHeight * 4);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = b.bmWidth;
    info.bmiHeader.biHeight = -b.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(nullptr);
    bool ok = GetDIBits(dc, bitmap, 0, b.bmHeight, out->bgra.data(), &info, DIB_RGB_COLORS) != 0;
    ReleaseDC(nullptr, dc);
    if (!ok)
        return {};
    bool alpha = false;
    for (size_t i = 3; i < out->bgra.size(); i += 4)
        if (out->bgra[i]) {
            alpha = true;
            break;
        }
    if (!alpha)
        for (size_t i = 3; i < out->bgra.size(); i += 4)
            out->bgra[i] = 255;
    return out;
}
struct Preview {
    std::wstring path, text;
    std::shared_ptr<const PixelImage> image;
};
struct TreeResult {
    std::wstring path, error;
    std::vector<std::pair<std::wstring, std::wstring>> children;
};
struct IconBatch {
    uint64_t revision = 0;
    std::vector<std::pair<int, std::shared_ptr<const PixelImage>>> images;
};
struct Tab {
    std::wstring path;
    std::vector<std::wstring> history;
    int historyAt = -1, scroll = 0;
    std::set<std::wstring> selection;
};
struct Browser;
struct DropHandler final : IDropTarget, IDropSource {
    LONG refs = 1;
    Browser *browser;
    bool acceptable = false;
    DWORD allowed = 0;
    explicit DropHandler(Browser *b) : browser(b) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDropTarget)
            *out = static_cast<IDropTarget *>(this);
        else if (id == IID_IDropSource)
            *out = static_cast<IDropSource *>(this);
        if (*out) {
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&refs);
        if (!r)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        return escape ? DRAGDROP_S_CANCEL : !(keys & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
struct Browser {
    HWND hwnd = nullptr, pathEdit = nullptr, renameEdit = nullptr, searchEdit = nullptr;
    WNDPROC oldPath = nullptr, oldRename = nullptr, oldSearch = nullptr;
    std::unique_ptr<Graphics> gfx;
    HBRUSH editBrush = nullptr;
    HFONT font = nullptr;
    DropHandler *drop = nullptr;
    float scale = 1, width = 1120, height = 720;
    int selected = -1, anchor = -1, scroll = 0, hover = -1, tabAt = 0, sortColumn = 0, dropIndex = -1, sideScroll = 0;
    bool operationPending = false;
    DWORD moveClipboardSequence = 0;
    std::vector<std::wstring> clipboardMoveSources;
    bool owned = false, drawing = false, resizing = false, showHidden = false, showExtensions = true, grid = false,
         descending = false, thumbnail = true, loading = false, dragPending = false, scrollDrag = false,
         previewEnabled = false, marquee = false;
    POINT dragStart{};
    Box marqueeBox{};
    std::set<int> marqueeInitial;
    std::wstring path, selectName, status, query, pendingOpen;
    std::vector<std::wstring> history, favorites;
    int historyAt = -1;
    std::vector<Tab> tabs;
    std::set<int> selection;
    std::set<std::wstring> preserve;
    std::shared_ptr<const Listing> listing = std::make_shared<Listing>();
    std::vector<int> visible;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false, pending = false;
    uint64_t revision = 0;
    std::wstring request, requestQuery;
    bool requestHidden = false, requestThumb = false, requestGrid = false;
    bool iconsPending = false;
    std::vector<int> iconRequest;
    std::shared_ptr<const Listing> iconListing;
    bool iconTiles = false, iconThumbs = false;
    std::deque<IconBatch> iconResults;
    std::map<int, std::shared_ptr<const PixelImage>> visuals;
    uint64_t visualRevision = 0;
    std::set<int> iconQueued;
    std::shared_ptr<const Listing> result;
    bool previewPending = false;
    std::wstring previewRequest;
    std::shared_ptr<const Preview> preview = std::make_shared<Preview>(), previewResult;
    files::Operations operations;
    std::vector<std::vector<files::Change>> &undo = undoHistory;
    std::vector<std::vector<files::Change>> &redo = redoHistory;
    int undoDirection = 0;
    std::vector<files::Change> pendingUndo;
    HANDLE changeEvent = INVALID_HANDLE_VALUE;
    ULONGLONG lastChange = 0;
    bool refreshPending = false;
    struct Hit {
        Box rect;
        int action;
    };
    std::vector<Hit> hits;
    std::vector<std::pair<std::wstring, std::wstring>> sidebar, crumbs;
    std::vector<int> sideDepth;
    std::vector<bool> sideFavorite;
    std::set<std::wstring> treeExpanded, treeLoading;
    std::map<std::wstring, TreeResult> treeCache;
    std::deque<std::wstring> treeRequests;
    std::deque<TreeResult> treeResults;
    bool sidebarDirty = true, addressEditing = false;
    float columnWidth[4] = {0, 154, 100, 104};
    unsigned columnMask = 15;
    int sizingColumn = -1;
    float columnStart = 0, columnOriginal = 0;
    ~Browser() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        wake.notify_one();
        if (worker.joinable())
            worker.join();
        if (changeEvent != INVALID_HANDLE_VALUE)
            FindCloseChangeNotification(changeEvent);
        if (editBrush)
            DeleteObject(editBrush);
        if (font)
            DeleteObject(font);
    }
    bool busy() const { return operationPending || operations.snapshot(false).busy; }
    float left() const { return width >= 800 ? 208.f : 16.f; }
    float right() const { return previewEnabled && width >= 1000 ? width - 300 : width; }
    int columns() const { return grid ? std::max(1, (int)((right() - left() - 22) / 112)) : 1; }
    int rows() const { return std::max(1, (int)((height - 222) / (grid ? 114 : 37))); }
    int capacity() const { return rows() * columns(); }
    Box itemBox(int pos) const {
        int v = pos - scroll;
        if (grid) {
            int c = columns();
            float w = (right() - left() - 22) / c;
            return {left() + (v % c) * w, 180 + (v / c) * 114.f, w - 6, 108};
        }
        return {left(), 180 + v * 37.f, right() - left() - 20, 35};
    }
    std::wstring shownName(const Entry &e) const {
        return showExtensions || e.folder ? e.name : fs::path(e.name).stem().wstring();
    }
    void savePreferences() {
        auto file = config();
        WritePrivateProfileStringW(L"view", L"columns", std::to_wstring(columnMask).c_str(), file.c_str());
        for (int i = 0; i < 4; i++)
            WritePrivateProfileStringW(L"view", (L"column" + std::to_wstring(i)).c_str(),
                                       std::to_wstring((int)columnWidth[i]).c_str(), file.c_str());
        auto f = config();
        for (auto [k, v] : std::vector<std::pair<const wchar_t *, int>>{{L"hidden", showHidden},
                                                                        {L"extensions", showExtensions},
                                                                        {L"grid", grid},
                                                                        {L"thumbnail", thumbnail},
                                                                        {L"sort", sortColumn},
                                                                        {L"descending", descending}})
            WritePrivateProfileStringW(L"view", k, std::to_wstring(v).c_str(), f.c_str());
    }
    void initPreferences() {
        auto f = config();
        HANDLE settings = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (settings != INVALID_HANDLE_VALUE) {
            WORD bom = 0xfeff;
            DWORD count;
            WriteFile(settings, &bom, 2, &count, nullptr);
            CloseHandle(settings);
        }
        showHidden = GetPrivateProfileIntW(L"view", L"hidden", 0, f.c_str()) != 0;
        showExtensions = GetPrivateProfileIntW(L"view", L"extensions", 1, f.c_str()) != 0;
        grid = GetPrivateProfileIntW(L"view", L"grid", 0, f.c_str()) != 0;
        thumbnail = GetPrivateProfileIntW(L"view", L"thumbnail", 1, f.c_str()) != 0;
        sortColumn = std::clamp((int)GetPrivateProfileIntW(L"view", L"sort", 0, f.c_str()), 0, 3);
        descending = GetPrivateProfileIntW(L"view", L"descending", 0, f.c_str()) != 0;
        columnMask = (GetPrivateProfileIntW(L"view", L"columns", 15, f.c_str()) & 15) | 1;
        for (int i = 0; i < 4; i++)
            columnWidth[i] = std::clamp((int)GetPrivateProfileIntW(L"view", (L"column" + std::to_wstring(i)).c_str(),
                                                                   (int)columnWidth[i], f.c_str()),
                                        i ? 64 : 0, 900);
        wchar_t p[32768];
        for (int i = 0; i < 16; i++) {
            GetPrivateProfileStringW(L"favorites", std::to_wstring(i).c_str(), L"", p, 32768, f.c_str());
            if (*p)
                favorites.emplace_back(p);
        }
    }
    void saveFavorites() {
        sidebarDirty = true;
        auto f = config();
        WritePrivateProfileStringW(L"favorites", nullptr, nullptr, f.c_str());
        for (size_t i = 0; i < favorites.size(); i++)
            WritePrivateProfileStringW(L"favorites", std::to_wstring(i).c_str(), favorites[i].c_str(), f.c_str());
    }
    void saveTab() {
        if (tabs.empty())
            return;
        auto &t = tabs[tabAt];
        t.path = path;
        t.history = history;
        t.historyAt = historyAt;
        t.scroll = scroll;
        t.selection.clear();
        for (auto p : selection)
            if (p >= 0 && p < (int)listing->entries.size())
                t.selection.insert(listing->entries[p].path);
    }
    void tab(int i) {
        if (i < 0 || i >= (int)tabs.size() || i == tabAt)
            return;
        saveTab();
        tabAt = i;
        auto t = tabs[i];
        history = t.history;
        historyAt = t.historyAt;
        load(t.path, false);
        preserve = t.selection;
        scroll = t.scroll;
    }
    void newTab(const std::wstring &p = L"") {
        saveTab();
        if (tabs.size() >= 12)
            return;
        tabs.push_back({});
        tabAt = (int)tabs.size() - 1;
        history.clear();
        historyAt = -1;
        load(p.empty() ? home() : p);
    }
    void closeTab(int i) {
        if (tabs.size() == 1) {
            if (!busy())
                DestroyWindow(hwnd);
            return;
        }
        if (i < 0 || i >= (int)tabs.size())
            return;
        saveTab();
        tabs.erase(tabs.begin() + i);
        if (tabAt > i)
            --tabAt;
        else if (tabAt == i) {
            tabAt = std::min(i, (int)tabs.size() - 1);
            auto t = tabs[tabAt];
            history = t.history;
            historyAt = t.historyAt;
            load(t.path, false);
            preserve = t.selection;
            scroll = t.scroll;
        }
        draw();
    }
    void watch() {
        if (changeEvent != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(changeEvent);
            changeEvent = INVALID_HANDLE_VALUE;
        }
        if (files::absolutePath(path))
            changeEvent = FindFirstChangeNotificationW(path.c_str(), FALSE,
                                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                           FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                                           FILE_NOTIFY_CHANGE_ATTRIBUTES);
    }
    void load(const std::wstring &next, bool remember = true) {
        std::wstring target = next.empty() ? home() : next;
        if (target.rfind(L"::", 0) != 0) {
            wchar_t expanded[32768] = {};
            DWORD n = ExpandEnvironmentStringsW(target.c_str(), expanded, 32768);
            if (n && n <= 32768)
                target = expanded;
            std::error_code ec;
            auto p = fs::absolute(fs::path(target), ec);
            if (ec || !fs::is_directory(p, ec)) {
                Com<IShellFolder> folder;
                if (!shellPath(target) || !bindFolder(target, folder.put())) {
                    status = L"Папка недоступна";
                    draw();
                    return;
                }
            }
            target = p.lexically_normal().wstring();
        }
        preserve.clear();
        path = target;
        addressEditing = false;
        ShowWindow(pathEdit, SW_HIDE);
        auto empty = std::make_shared<Listing>();
        empty->path = target;
        listing = empty;
        visible.clear();
        SetPropW(hwnd, L"OwnIsland.Files.Count", nullptr);
        SetPropW(hwnd, L"OwnIsland.Files.RenderedCount", nullptr);
        scroll = 0;
        selected = anchor = -1;
        selection.clear();
        hover = -1;
        query.clear();
        SetWindowTextW(searchEdit, L"");
        ShowWindow(renameEdit, SW_HIDE);
        status.clear();
        if (remember) {
            history.resize(historyAt + 1);
            history.push_back(path);
            historyAt = (int)history.size() - 1;
        }
        SetWindowTextW(pathEdit, path.rfind(L"::", 0) == 0 ? label(path).c_str() : path.c_str());
        SetWindowTextW(hwnd, (L"Файлы — " + label(path)).c_str());
        if (!tabs.empty())
            tabs[tabAt].path = path;
        watch();
        requestListing();
    }
    void requestListing() {
        loading = true;
        {
            std::lock_guard lock(mutex);
            request = path;
            requestQuery = query;
            requestHidden = showHidden;
            requestThumb = thumbnail;
            requestGrid = grid;
            pending = true;
            ++revision;
        }
        wake.notify_one();
        draw();
    }
    void refresh() {
        preserve.clear();
        for (auto i : selection)
            if (i >= 0 && i < (int)listing->entries.size())
                preserve.insert(listing->entries[i].path);
        requestListing();
    }
    void run() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::map<std::wstring, std::shared_ptr<const PixelImage>> cache;
        for (;;) {
            std::wstring next, search;
            uint64_t version;
            bool hidden, thumb, tiles;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,
                          [&] { return stop || pending || previewPending || iconsPending || !treeRequests.empty(); });
                if (stop)
                    break;
                if (!pending && !treeRequests.empty()) {
                    auto branch = treeRequests.front();bool treeHidden=requestHidden;
                    treeRequests.pop_front();
                    lock.unlock();
                    TreeResult tree;
                    tree.path = branch;
                    if (branch == L"::computer") {
                        wchar_t drives[512] = {};
                        GetLogicalDriveStringsW(512, drives);
                        for (auto p = drives; *p; p += wcslen(p) + 1)
                            tree.children.push_back({p, p});
                    } else {
                        std::error_code ec;
                        fs::directory_iterator it(branch, fs::directory_options::skip_permission_denied, ec), end;
                        for (; it != end; it.increment(ec)) {
                            if (ec) {
                                tree.error = files::errorText(HRESULT_FROM_WIN32(ec.value()));
                                break;
                            }
                            {
                                std::lock_guard state(mutex);
                                if (stop)
                                    break;
                            }
                            DWORD a = GetFileAttributesW(it->path().c_str());
                            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
                                (treeHidden||!(a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))))
                                tree.children.push_back({it->path().filename().wstring(), it->path().wstring()});
                            if (tree.children.size() >= 2048)
                                break;
                        }
                        if (ec)
                            tree.error = files::errorText(HRESULT_FROM_WIN32(ec.value()));
                    }
                    std::sort(tree.children.begin(), tree.children.end(),
                              [](auto &a, auto &b) { return StrCmpLogicalW(a.first.c_str(), b.first.c_str()) < 0; });
                    {
                        std::lock_guard state(mutex);
                        treeResults.push_back(std::move(tree));
                    }
                    PostMessageW(hwnd, WM_APP + 7, 0, 0);
                    continue;
                }
                if (!pending && !previewPending && iconsPending) {
                    auto indexes = std::move(iconRequest);
                    auto source = iconListing;
                    bool tiles = iconTiles, thumb = iconThumbs;
                    iconsPending = false;
                    lock.unlock();
                    IconBatch batch;
                    batch.revision = source->revision;
                    for (int index : indexes) {
                        {
                            std::lock_guard state(mutex);
                            if (stop || revision != source->revision)
                                break;
                        }
                        auto &entry = source->entries[index];
                        auto ext = lower(fs::path(entry.path).extension().wstring());
                        std::wstring key = entry.folder                                           ? L"<folder>"
                                           : (ext == L".exe" || ext == L".lnk" || ext == L".ico") ? entry.path
                                                                                                  : ext;
                        std::shared_ptr<const PixelImage> icon;
                        bool picture = tiles && thumb &&
                                       (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp" ||
                                        ext == L".gif" || ext == L".webp");
                        if (picture) {
                            Com<IShellItemImageFactory> factory;
                            if (SUCCEEDED(SHCreateItemFromParsingName(entry.path.c_str(), nullptr,
                                                                      IID_PPV_ARGS(factory.put())))) {
                                HBITMAP bitmap = nullptr;
                                if (SUCCEEDED(factory->GetImage({96, 96}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK,
                                                                &bitmap))) {
                                    icon = bitmapPixels(bitmap);
                                    DeleteObject(bitmap);
                                }
                            }
                        }
                        if (!icon) {
                            auto found = cache.find(key);
                            if (found != cache.end())
                                icon = found->second;
                            else {
                                icon = loadShellIcon(entry.path, ++nextImage);
                                if (cache.size() < 512)
                                    cache[key] = icon;
                            }
                        }
                        batch.images.push_back({index, icon});
                        if (batch.images.size() == 16) {
                            {
                                std::lock_guard state(mutex);
                                iconResults.push_back(std::move(batch));
                            }
                            PostMessageW(hwnd, WM_APP + 6, 0, 0);
                            batch = {};
                            batch.revision = source->revision;
                        }
                    }
                    if (!batch.images.empty()) {
                        std::lock_guard state(mutex);
                        iconResults.push_back(std::move(batch));
                        PostMessageW(hwnd, WM_APP + 6, 0, 0);
                    }
                    continue;
                }
                if (!pending && previewPending) {
                    auto previewPath = previewRequest;
                    previewPending = false;
                    lock.unlock();
                    auto value = std::make_shared<Preview>();
                    value->path = previewPath;
                    auto ext = lower(fs::path(previewPath).extension().wstring());
                    if (ext == L".txt" || ext == L".md" || ext == L".json" || ext == L".log" || ext == L".ini" ||
                        ext == L".csv" || ext == L".xml" || ext == L".cpp" || ext == L".h" || ext == L".cs") {
                        std::ifstream input(fs::path(previewPath), std::ios::binary);
                        std::string bytes(65536, 0);
                        input.read(bytes.data(), bytes.size());
                        bytes.resize((size_t)input.gcount());
                        if (bytes.size() > 1 && (unsigned char)bytes[0] == 255 && (unsigned char)bytes[1] == 254) {
                            value->text.assign((const wchar_t *)(bytes.data() + 2), (bytes.size() - 2) / 2);
                        } else {
                            if (bytes.size() > 2 && (unsigned char)bytes[0] == 239 && (unsigned char)bytes[1] == 187 &&
                                (unsigned char)bytes[2] == 191)
                                bytes.erase(0, 3);
                            UINT code = CP_UTF8;
                            int count = MultiByteToWideChar(code, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(),
                                                            nullptr, 0);
                            if (!count) {
                                code = CP_ACP;
                                count = MultiByteToWideChar(code, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
                            }
                            value->text.resize(count);
                            MultiByteToWideChar(code, 0, bytes.data(), (int)bytes.size(), value->text.data(), count);
                        }
                    } else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" ||
                               ext == L".gif" || ext == L".webp") {
                        Com<IShellItemImageFactory> factory;
                        if (SUCCEEDED(SHCreateItemFromParsingName(previewPath.c_str(), nullptr,
                                                                  IID_PPV_ARGS(factory.put())))) {
                            HBITMAP bitmap = nullptr;
                            if (SUCCEEDED(factory->GetImage({512, 512}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK,
                                                            &bitmap))) {
                                value->image = bitmapPixels(bitmap);
                                DeleteObject(bitmap);
                            }
                        }
                    }
                    lock.lock();
                    if (previewRequest == previewPath)
                        previewResult = value;
                    lock.unlock();
                    PostMessageW(hwnd, WM_APP + 4, 0, 0);
                    continue;
                }
                next = request;
                search = lower(requestQuery);
                version = revision;
                hidden = requestHidden;
                thumb = requestThumb;
                tiles = requestGrid;
                pending = false;
            }
            auto list = std::make_shared<Listing>();
            list->path = next;
            list->revision = version;
            list->search = !search.empty();
            std::error_code ec;
            auto outdated = [&] {
                std::lock_guard lock(mutex);
                return stop || version != revision;
            };
            auto append = [&](const std::wstring &p, const std::wstring &display = L"") {
                WIN32_FILE_ATTRIBUTE_DATA a{};
                Entry e;
                e.path = p;
                e.name = display.empty()        ? fs::path(p).filename().wstring()
                         : next == L"::recycle" ? fs::path(display).filename().wstring()
                                                : display;
                if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) {
                    e.attributes = a.dwFileAttributes;
                    e.folder = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    e.bytes = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
                    e.modified = a.ftLastWriteTime;
                    if (next != L"::recycle" && !hidden &&
                        (e.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                        return;
                } else if (next == L"::computer" || next == L"::recycle" || next == L"::network" || shellPath(next)) {
                    Com<IShellItem> item;
                    if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
                        SFGAOF attrs = 0;
                        item->GetAttributes(SFGAO_FOLDER, &attrs);
                        e.folder = (attrs & SFGAO_FOLDER) != 0;
                    }
                } else
                    return;
                auto ext = lower(fs::path(e.name).extension().wstring());
                e.type = e.folder ? L"Папка" : ext.empty() ? L"Файл" : ext.substr(1);
                if (!search.empty() && lower(e.name).find(search) == std::wstring::npos)
                    return;
                list->entries.push_back(std::move(e));
            };
            if (next == L"::computer" || next == L"::recycle" || next == L"::network" || shellPath(next)) {
                Com<IShellFolder> root, folder;
                Com<IEnumIDList> items;
                PIDLIST_ABSOLUTE pidl = nullptr;
                HRESULT hr = next == L"::computer" || next == L"::recycle" || next == L"::network"
                                 ? SHGetKnownFolderIDList(next == L"::computer"  ? FOLDERID_ComputerFolder
                                                          : next == L"::recycle" ? FOLDERID_RecycleBinFolder
                                                                                 : FOLDERID_NetworkFolder,
                                                          0, nullptr, &pidl)
                                 : SHParseDisplayName(next.c_str(), nullptr, &pidl, 0, nullptr);
                if (SUCCEEDED(hr) && SUCCEEDED(SHGetDesktopFolder(root.put())))
                    hr = root->BindToObject(pidl, nullptr, IID_PPV_ARGS(folder.put()));
                if (SUCCEEDED(hr) && folder)
                    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, items.put());
                if (FAILED(hr))
                    list->error = files::errorText(hr);
                PITEMID_CHILD child = nullptr;
                while (items && items->Next(1, &child, nullptr) == S_OK) {
                    auto full = ILCombine(pidl, child);
                    Com<IShellItem> item;
                    if (full && SUCCEEDED(SHCreateItemFromIDList(full, IID_PPV_ARGS(item.put())))) {
                        PWSTR p = nullptr, n = nullptr;
                        item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p);
                        item->GetDisplayName(SIGDN_NORMALDISPLAY, &n);
                        if (p) {
                            auto count = list->entries.size();
                            append(p, n ? n : L"");
                            if (list->entries.size() > count && full) {
                                auto bytes = (const BYTE *)full;
                                list->entries.back().shellId.assign(bytes, bytes + ILGetSize(full));
                            }
                        }
                        CoTaskMemFree(p);
                        CoTaskMemFree(n);
                    }
                    CoTaskMemFree(full);
                    CoTaskMemFree(child);
                    if (outdated())
                        break;
                    if (list->entries.size() >= 20000) {
                        list->truncated = true;
                        break;
                    }
                }
                CoTaskMemFree(pidl);
            } else if (search.empty()) {
                fs::directory_iterator it(next, ec), end;
                if (ec)
                    list->error = files::errorText(HRESULT_FROM_WIN32(ec.value()));
                for (; it != end; it.increment(ec)) {
                    if (ec) {
                        list->error = files::errorText(HRESULT_FROM_WIN32(ec.value()));
                        break;
                    }
                    if (outdated())
                        break;
                    append(it->path().wstring());
                    if (list->entries.size() >= 100000) {
                        list->truncated = true;
                        break;
                    }
                }
            } else {
                auto found = searchFolder(search, next);
                if (found.indexError)
                    list->error = L"Поиск Windows недоступен";
                for (auto &e : found.results) {
                    if (outdated())
                        break;
                    append(e.path);
                }
                list->truncated = found.results.size() >= 2048;
            }
            {
                std::lock_guard lock(mutex);
                if (stop)
                    break;
                if (version != revision)
                    continue;
                result = list;
            }
            PostMessageW(hwnd, WM_APP + 1, 0, 0);
        }
        CoUninitialize();
    }
    void treeToggle(int i) {
        if (i < 0 || i >= (int)sidebar.size() || sideDepth[i] >= 12)
            return;
        auto p = sidebar[i].second;
        if (p == L"::recycle" || p == L"::network")
            return;
        if (treeExpanded.erase(p) == 0) {
            treeExpanded.insert(p);
            if (!treeCache.count(p) && !treeLoading.count(p)) {
                treeLoading.insert(p);
                {
                    std::lock_guard lock(mutex);
                    treeRequests.push_back(p);
                }
                wake.notify_one();
            }
        }
        sidebarDirty = true;
        draw();
    }
    void buildSidebar() {
        if (!sidebarDirty)
            return;
        sidebarDirty = false;
        sidebar.clear();
        sideDepth.clear();
        sideFavorite.clear();
        std::vector<std::pair<std::wstring, std::wstring>> roots = {{L"Этот компьютер", L"::computer"},
                                                                    {L"Главная", home()},
                                                                    {L"Рабочий стол", known(FOLDERID_Desktop)},
                                                                    {L"Загрузки", known(FOLDERID_Downloads)},
                                                                    {L"Документы", known(FOLDERID_Documents)},
                                                                    {L"Изображения", known(FOLDERID_Pictures)},
                                                                    {L"Музыка", known(FOLDERID_Music)},
                                                                    {L"Видео", known(FOLDERID_Videos)},
                                                                    {L"Корзина", L"::recycle"},
                                                                    {L"Сеть", L"::network"}};
        for (auto &p : favorites)
            roots.push_back({label(p), p});
        std::function<void(const std::pair<std::wstring, std::wstring> &, int, bool)> add = [&](auto &node, int depth,
                                                                                                bool favorite) {
            if (sidebar.size() >= 4096)
                return;
            sidebar.push_back(node);
            sideDepth.push_back(depth);
            sideFavorite.push_back(favorite);
            if (depth < 12 && treeExpanded.count(node.second)) {
                auto found = treeCache.find(node.second);
                if (found != treeCache.end())
                    for (auto &child : found->second.children)
                        add(child, depth + 1, false);
            }
        };
        for (int i = 0; i < (int)roots.size(); i++)
            add(roots[i], 0, i >= 10);
    }
    void beginAddress() {
        addressEditing = true;
        ShowWindow(pathEdit, SW_SHOW);
        SetFocus(pathEdit);
        SendMessageW(pathEdit, EM_SETSEL, 0, -1);
    }
    void drawCrumbs(float available) {
        crumbs.clear();
        if (path.rfind(L"::", 0) == 0 || path.rfind(L"shell:", 0) == 0)
            crumbs.push_back({label(path), path});
        else {
            fs::path p(path), at = p.root_path();
            if (!at.empty())
                crumbs.push_back({at.wstring(), at.wstring()});
            for (const auto &part : p.relative_path()) {
                at /= part;
                crumbs.push_back({part.wstring(), at.wstring()});
            }
        }
        hits.push_back({{158, 57, available, 34}, 50});
        if (addressEditing)
            return;
        std::vector<int> show;
        float total = 0;
        for (int i = 0; i < (int)crumbs.size(); i++)
            total += std::min(180.f, gfx->textWidth(crumbs[i].first, 11, L"Segoe UI") + 28);
        if (total <= available - 12) {
            for (int i = 0; i < (int)crumbs.size(); i++)
                show.push_back(i);
        } else {
            if (!crumbs.empty())
                show.push_back(0);
            float remaining = available - 74;
            std::vector<int> tail;
            for (int i = (int)crumbs.size() - 1; i > 0; i--) {
                float w = std::min(180.f, gfx->textWidth(crumbs[i].first, 11, L"Segoe UI") + 28);
                if (w > remaining && !tail.empty())
                    break;
                tail.push_back(i);
                remaining -= w;
            }
            std::reverse(tail.begin(), tail.end());
            if (!tail.empty() && tail.front() > 1)
                show.push_back(-1);
            for (auto i : tail)
                show.push_back(i);
        }
        float x = 168;
        for (int i : show) {
            float w = i < 0 ? 24 : std::min(180.f, gfx->textWidth(crumbs[i].first, 11, L"Segoe UI") + 24);
            w = std::min(w, 158 + available - x - 4);
            if (w < 10)
                break;
            gfx->text(i < 0 ? L"…" : crumbs[i].first, {x, 57, w - 10, 34}, 11, palette.text);
            gfx->line(x + w - 8, 71, x + w - 5, 74, palette.muted);
            gfx->line(x + w - 5, 74, x + w - 8, 77, palette.muted);
            hits.push_back({{x, 57, w, 34}, i < 0 ? 51 : 30000 + i});
            x += w;
        }
    }
    std::array<Box, 4> columnBoxes() const {
        std::array<Box, 4> boxes{};
        float room = right() - left() - 64, fixed = 0, minima[] = {120, 100, 64, 76};
        for (int i = 1; i < 4; i++)
            if (columnMask & (1u << i))
                fixed += columnWidth[i];
        float ratio = fixed > room - 120 ? (room - 120) / std::max(1.f, fixed) : 1;
        float other = 0;
        for (int i = 1; i < 4; i++)
            if (columnMask & (1u << i)) {
                boxes[i].w = std::max(minima[i], columnWidth[i] * ratio);
                other += boxes[i].w;
            }
        boxes[0].w = std::max(120.f, columnWidth[0] > 0 ? std::min(columnWidth[0], room - other) : room - other);
        float x = left() + 44;
        for (int i = 0; i < 4; i++) {
            boxes[i].x = x;
            boxes[i].y = 146;
            boxes[i].h = 29;
            if (i == 0 || (columnMask & (1u << i)))
                x += boxes[i].w;
        }
        return boxes;
    }
    int columnEdge(float x, float y) const {
        if (grid || y < 146 || y > 177)
            return -1;
        auto boxes = columnBoxes();
        for (int i = 0; i < 4; i++)
            if ((columnMask & (1u << i)) && std::abs(x - boxes[i].x - boxes[i].w) < 5)
                return i;
        return -1;
    }
    void columnsMenu(POINT p) {
        int n = customMenu(hwnd, p,
                           {{L"Изменён", nullptr, 61, (columnMask & 2) != 0},
                            {L"Тип", nullptr, 62, (columnMask & 4) != 0},
                            {L"Размер", nullptr, 63, (columnMask & 8) != 0},
                            {L"Сбросить ширину", nullptr, 64}},
                           palette);
        if (n >= 61 && n <= 63)
            columnMask ^= 1u << (n - 60);
        if (n == 64) {
            columnWidth[0] = 0;
            columnWidth[1] = 154;
            columnWidth[2] = 100;
            columnWidth[3] = 104;
        }
        savePreferences();
        draw();
    }
    void requestIcons() {
        if (loading || listing->revision != revision)
            return;
        std::vector<int> wanted;
        for (int pos = std::max(0, scroll);
             pos < (int)visible.size() && pos < scroll + capacity() + columns() * 2 && wanted.size() < 256; pos++) {
            int i = visible[pos];
            if (!visuals.count(i) && !iconQueued.count(i))
                wanted.push_back(i);
        }
        if (wanted.empty())
            return;
        {
            std::lock_guard lock(mutex);
            if (iconsPending)
                for (auto i : iconRequest)
                    iconQueued.erase(i);
            iconRequest = std::move(wanted);
            for (auto i : iconRequest)
                iconQueued.insert(i);
            iconListing = listing;
            iconTiles = grid;
            iconThumbs = thumbnail;
            iconsPending = true;
        }
        wake.notify_one();
    }
    void requestPreview() {
        if (!previewEnabled)
            return;
        std::wstring p =
            selected >= 0 && selected < (int)listing->entries.size() ? listing->entries[selected].path : L"";
        {
            std::lock_guard lock(mutex);
            if (previewRequest == p)
                return;
            previewRequest = p;
            previewPending = true;
        }
        wake.notify_one();
    }
    void rebuild() {
        visible.clear();
        for (int i = 0; i < (int)listing->entries.size(); i++)
            visible.push_back(i);
        std::stable_sort(visible.begin(), visible.end(), [&](int x, int y) {
            auto &a = listing->entries[x];
            auto &b = listing->entries[y];
            if (a.folder != b.folder)
                return a.folder > b.folder;
            int n = 0;
            if (sortColumn == 1)
                n = CompareFileTime(&a.modified, &b.modified);
            else if (sortColumn == 2)
                n = StrCmpLogicalW(a.type.c_str(), b.type.c_str());
            else if (sortColumn == 3)
                n = a.bytes < b.bytes ? -1 : a.bytes > b.bytes ? 1 : 0;
            if (!n)
                n = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
            return descending ? n > 0 : n < 0;
        });
    }
    void fonts() {
        if (font)
            DeleteObject(font);
        font = CreateFontW(-(int)(14 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (auto w : {pathEdit, renameEdit, searchEdit})
            if (w)
                SendMessageW(w, WM_SETFONT, (WPARAM)font, TRUE);
    }
    void resize() {
        if (drawing || resizing) {
            SetTimer(hwnd, 5, 100, nullptr);
            return;
        }
        resizing = true;
        RECT r{};
        GetClientRect(hwnd, &r);
        float old = scale;
        scale = GetDpiForWindow(hwnd) / 96.f;
        width = r.right / scale;
        height = r.bottom / scale;
        if (scale != old)
            fonts();
        if (!gfx) {
            gfx = std::make_unique<Graphics>();
            if (!gfx->init(hwnd, r.right, r.bottom, scale, true)) {
                SetPropW(hwnd, L"OwnIsland.Files.ErrorStage", (HANDLE)1);
                SetPropW(hwnd, L"OwnIsland.Files.GraphicsError", (HANDLE)(UINT_PTR)(DWORD)gfx->result());
                gfx.reset();
            }
        } else if (!gfx->resize(r.right, r.bottom, scale)) {
            SetPropW(hwnd, L"OwnIsland.Files.ErrorStage", (HANDLE)2);
            SetPropW(hwnd, L"OwnIsland.Files.GraphicsError", (HANDLE)(UINT_PTR)(DWORD)gfx->result());
            gfx.reset();
        }
        SetPropW(hwnd, L"OwnIsland.Files.Graphics", (HANDLE)(UINT_PTR)(gfx ? 1 : 0));
        float sw = width >= 900 ? 220 : 180;
        if (pathEdit)
            SetWindowPos(pathEdit, nullptr, (int)(168 * scale), (int)(64 * scale), (int)((width - sw - 214) * scale),
                         (int)(23 * scale), SWP_NOZORDER | SWP_NOACTIVATE);
        if (searchEdit)
            SetWindowPos(searchEdit, nullptr, (int)((width - sw + 2) * scale), (int)(64 * scale),
                         (int)((sw - 34) * scale), (int)(23 * scale), SWP_NOZORDER | SWP_NOACTIVATE);
        resizing = false;
        draw();
    }
    void button(Box b, const wchar_t *glyph, const wchar_t *title, int action, bool active = false) {
        if (!gfx)
            return;
        gfx->fill(b, active ? palette.accent : palette.card, 7, active ? .25f : 1);
        if (glyph)
            gfx->icon(glyph, {title ? b.x + 8 : b.x + (b.w - 23) / 2, b.y, 23, b.h}, 14,
                      active ? palette.text : palette.muted);
        if (title)
            gfx->text(title, {b.x + (glyph ? 36.f : 10.f), b.y, b.w - (glyph ? 44.f : 20.f), b.h}, 11, palette.text);
        hits.push_back({b, action});
    }
    void draw() {
        if (!gfx || drawing || resizing)
            return;
        drawing = true;
        gfx->begin();
        gfx->fill({0, 0, width, height}, palette.background);
        hits.clear();
        float tw = std::min(194.f, (width - 208) / std::max(1, (int)tabs.size()));
        for (int i = 0; i < (int)tabs.size(); i++) {
            float x = 16 + i * tw;
            Box b{x, 8, tw - 5, 34};
            gfx->fill(b, i == tabAt ? palette.card : palette.background, 7);
            gfx->text(label(tabs[i].path), {x + 12, 8, tw - 48, 34}, 11, palette.text);
            hits.push_back({b, 1000 + i});
            gfx->icon(L"\uE8BB", {x + tw - 30, 8, 22, 34}, 10, palette.muted);
            hits.push_back({{x + tw - 30, 8, 24, 34}, 1100 + i});
        }
        button({16 + tabs.size() * tw, 8, 30, 34}, L"\uE710", nullptr, 11);
        button({width - 132, 8, 36, 30}, L"\uE921", nullptr, 101);
        button({width - 90, 8, 36, 30}, L"\uE922", nullptr, 102);
        button({width - 48, 8, 36, 30}, L"\uE8BB", nullptr, 103);
        button({16, 57, 38, 34}, L"\uE72B", nullptr, 1);
        button({62, 57, 38, 34}, L"\uE72A", nullptr, 10);
        button({108, 57, 38, 34}, L"\uE74A", nullptr, 2);
        float sw = width >= 900 ? 220 : 180;
        gfx->fill({158, 57, width - sw - 194, 34}, palette.card, 7);
        drawCrumbs(width - sw - 194);
        gfx->fill({width - sw - 8, 57, sw - 12, 34}, palette.card, 7);
        button({16, 103, 136, 32}, L"\uE8F4", L"Новая папка", 4);
        button({160, 103, 122, 32}, L"\uE8C8", L"Копировать", 5);
        button({290, 103, 116, 32}, L"\uE77F", L"Вставить", 6);
        button({414, 103, 38, 32}, L"\uE72C", nullptr, 7);
        button({460, 103, 38, 32}, L"\uE74D", nullptr, 8);
        button({506, 103, 38, 32}, L"\uE7A7", nullptr, 20);
        button({552, 103, 96, 32}, L"\uE8A9", L"Вид", 12);
        button({656, 103, 38, 32}, L"\uE712", nullptr, 13);
        buildSidebar();
        if (left() > 20) {
            float y = 146;
            sideScroll = std::clamp(sideScroll, 0, std::max(0, (int)sidebar.size() - (int)((height - 218) / 32)));
            for (int i = sideScroll; i < (int)sidebar.size() && y < height - 72; i++, y += 32) {
                auto &s = sidebar[i];
                if (s.second == path)
                    gfx->fill({16, y, 176, 29}, palette.card, 6);
                float indent = sideDepth[i] * 12.f;
                bool expandable = s.second != L"::recycle" && s.second != L"::network" && sideDepth[i] < 12;
                bool expanded = treeExpanded.count(s.second) != 0;
                float cx = 24 + indent;
                if (expandable) {
                    if (expanded) {
                        gfx->line(cx - 3, y + 12, cx, y + 15, palette.muted);
                        gfx->line(cx, y + 15, cx + 3, y + 12, palette.muted);
                    } else {
                        gfx->line(cx - 2, y + 10, cx + 1, y + 13, palette.muted);
                        gfx->line(cx + 1, y + 13, cx - 2, y + 16, palette.muted);
                    }
                }
                if (s.second == L"::recycle")
                    gfx->icon(L"\uE74D", {32 + indent, y, 20, 29}, 14, palette.muted);
                else if (s.second == L"::computer") {
                    gfx->stroke({33 + indent, y + 7, 16, 12}, palette.muted, 1, 1);
                    gfx->line(41 + indent, y + 19, 41 + indent, y + 22, palette.muted);
                    gfx->line(36 + indent, y + 23, 46 + indent, y + 23, palette.muted);
                } else {
                    gfx->fill({33 + indent, y + 8, 8, 4}, palette.muted, 1);
                    gfx->fill({33 + indent, y + 11, 17, 11}, palette.muted, 2);
                }
                gfx->text(s.first, {57 + indent, y, std::max(20.f, 129 - indent), 29}, 11, palette.text);
                hits.push_back({{16, y, 176, 29}, 10000 + i});
                if (expandable)
                    hits.push_back({{16 + indent, y, 16, 29}, 20000 + i});
            }
            gfx->line(198, 150, 198, height - 44, palette.card);
        }
        float available = right() - left() - 20;
        auto col = columnBoxes();
        if (grid)
            gfx->text(listing->search ? L"Результаты поиска" : label(path), {left() + 4, 146, available, 29}, 11,
                      palette.muted);
        else {
            std::wstring heads[] = {L"Имя", L"Изменён", L"Тип", L"Размер"};
            for (int i = 0; i < 4; i++)
                if (columnMask & (1u << i)) {
                    gfx->text(heads[i] + (sortColumn == i ? (descending ? L" ↓" : L" ↑") : L""),
                              {col[i].x, 146, col[i].w - 12, 29}, 11, palette.muted);
                    gfx->line(col[i].x + col[i].w - 3, 153, col[i].x + col[i].w - 3, 169, palette.card);
                    hits.push_back({col[i], 3000 + i});
                }
        }
        gfx->line(left(), 177, right() - 20, 177, palette.card);
        int count = capacity(), cols = columns(),
            maxScroll = std::max(0, ((int)visible.size() + cols - 1) / cols - rows()) * cols;
        scroll = std::clamp(scroll, 0, maxScroll);
        if (grid)
            scroll = scroll / cols * cols;
        requestIcons();
        for (int pos = scroll; pos < (int)visible.size() && pos < scroll + count; pos++) {
            int i = visible[pos];
            auto &e = listing->entries[i];
            Box b = itemBox(pos);
            if (selection.count(i))
                gfx->fill(b, palette.card, 7);
            else if (i == hover || i == dropIndex)
                gfx->fill(b, palette.card, 7, .5f);
            Box icon = grid ? Box{b.x + (b.w - 64) / 2, b.y + 7, 64, 64} : Box{b.x + 8, b.y + 5, 25, 25};
            auto visual = visuals.find(i);
            if (visual != visuals.end() && visual->second)
                gfx->sprite(visual->second, icon);
            else if (e.folder) {
                gfx->fill({icon.x + 2, icon.y + 3, icon.w * .45f, icon.h * .28f}, palette.accent, 2);
                gfx->fill({icon.x + 2, icon.y + icon.h * .25f, icon.w - 4, icon.h * .65f}, palette.accent, 3);
            } else
                gfx->stroke({icon.x + 5, icon.y + 2, icon.w - 10, icon.h - 4}, palette.muted, 2, 1);
            if (grid)
                gfx->text(shownName(e), {b.x + 7, b.y + 76, b.w - 14, 29}, 11, palette.text, false,
                          DWRITE_TEXT_ALIGNMENT_CENTER, true);
            else {
                gfx->text(shownName(e), {col[0].x, b.y, col[0].w - 12, b.h}, 12,
                          (e.attributes & FILE_ATTRIBUTE_HIDDEN) ? palette.muted : palette.text);
                if (columnMask & 2)
                    gfx->text(dateText(e.modified), {col[1].x, b.y, col[1].w - 12, b.h}, 10, palette.muted);
                if (columnMask & 4)
                    gfx->text(e.type, {col[2].x, b.y, col[2].w - 12, b.h}, 10, palette.muted);
                if ((columnMask & 8) && !e.folder)
                    gfx->text(sizeText(e.bytes), {col[3].x, b.y, col[3].w - 12, b.h}, 11, palette.muted, false,
                              DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
        if (visible.empty() && !loading)
            gfx->text(!listing->error.empty() ? listing->error
                      : query.empty()         ? L"Папка пуста"
                                              : L"Ничего не найдено",
                      {left() + 24, 200, available - 48, 80}, 13, palette.muted, false, DWRITE_TEXT_ALIGNMENT_CENTER,
                      true);
        if ((int)visible.size() > count) {
            float track = height - 226, thumb = std::max(24.f, track * count / visible.size());
            gfx->fill({right() - 9, 181, 3, track}, palette.card, 1);
            gfx->fill({right() - 9, 181 + (track - thumb) * scroll / std::max(1, maxScroll), 3, thumb}, palette.muted,
                      1);
        }
        if (marquee)
            gfx->stroke(marqueeBox, palette.accent, 2, 1);
        if (right() != width) {
            float x = right() + 12;
            gfx->line(right(), 150, right(), height - 44, palette.card);
            gfx->text(preview->path.empty() ? L"Просмотр" : label(preview->path), {x, 150, 264, 30}, 12, palette.text,
                      true);
            if (preview->image)
                gfx->sprite(preview->image, {x, 195, 264, std::min(330.f, height - 250)});
            else if (!preview->text.empty())
                gfx->text(preview->text, {x, 195, 264, height - 246}, 11, palette.text, false,
                          DWRITE_TEXT_ALIGNMENT_LEADING, true, L"Consolas", DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            else
                gfx->text(L"Нет предпросмотра", {x, 200, 264, 45}, 11, palette.muted);
        }
        auto progress = operations.snapshot(false);
        std::wstring footer =
            !status.empty() ? status
            : loading       ? L"Загрузка…"
                            : std::to_wstring(visible.size()) + (listing->truncated ? L"+ элементов" : L" элементов");
        if (!selection.empty()) {
            uintmax_t bytes = 0;
            for (auto i : selection)
                if (i >= 0 && i < (int)listing->entries.size())
                    bytes += listing->entries[i].bytes;
            footer += L"  •  Выбрано: " + std::to_wstring(selection.size()) + L"  " + sizeText(bytes);
        }
        if (progress.busy) {
            footer = L"Выполняется: " + fs::path(progress.name).filename().wstring();
            if (progress.total)
                footer += L"  " + std::to_wstring(progress.done) + L" / " + std::to_wstring(progress.total);
            button({width - 112, height - 35, 92, 27}, nullptr, L"Отменить", 21);
        }
        gfx->text(footer, {20, height - 34, width - (progress.busy ? 146 : 40), 28}, 11, palette.muted);
        if (!gfx->end()) {
            SetPropW(hwnd, L"OwnIsland.Files.ErrorStage", (HANDLE)3);
            SetPropW(hwnd, L"OwnIsland.Files.GraphicsError", (HANDLE)(UINT_PTR)(DWORD)gfx->result());
            gfx.reset();
            SetTimer(hwnd, 5, 250, nullptr);
        } else {
            SetPropW(hwnd, L"OwnIsland.Files.RenderedCount", (HANDLE)(UINT_PTR)(listing->entries.size() + 1));
            SetPropW(hwnd, L"OwnIsland.Files.RenderedRevision", (HANDLE)(UINT_PTR)listing->revision);
        }
        SetPropW(hwnd, L"OwnIsland.Files.Selection", (HANDLE)(UINT_PTR)selection.size());
        SetPropW(hwnd, L"OwnIsland.Files.Tabs", (HANDLE)(UINT_PTR)tabs.size());
        SetPropW(hwnd, L"OwnIsland.Files.Busy", (HANDLE)(UINT_PTR)busy());
        SetPropW(hwnd, L"OwnIsland.Files.Column0Width", (HANDLE)(UINT_PTR)(int)columnBoxes()[0].w);
        SetPropW(hwnd, L"OwnIsland.Files.CrumbsCount", (HANDLE)(UINT_PTR)crumbs.size());
        SetPropW(hwnd, L"OwnIsland.Files.PreviewReady",
                 (HANDLE)(UINT_PTR)(!preview->path.empty() && (!preview->text.empty() || preview->image)));
        SetPropW(hwnd, L"OwnIsland.Files.ColumnMask", (HANDLE)(UINT_PTR)columnMask);
        SetPropW(hwnd, L"OwnIsland.Files.Loading", (HANDLE)(UINT_PTR)loading);
        drawing = false;
    }
    int rowAt(int x, int y) const {
        float px = x / scale, py = y / scale;
        if (px < left() || px >= right() - 18 || py < 180 || py >= height - 42)
            return -1;
        int p = scroll + (grid ? (int)((py - 180) / 114) * columns() +
                                     (int)((px - left()) / ((right() - left() - 22) / columns()))
                               : (int)((py - 180) / 37));
        return p >= 0 && p < (int)visible.size() ? visible[p] : -1;
    }
    int position(int index) const {
        auto it = std::find(visible.begin(), visible.end(), index);
        return it == visible.end() ? -1 : (int)(it - visible.begin());
    }
    void ensureVisible() {
        int p = position(selected);
        if (p < 0)
            return;
        if (p < scroll)
            scroll = p / columns() * columns();
        else if (p >= scroll + capacity())
            scroll = (p / columns() - rows() + 1) * columns();
    }
    void choose(int i, bool ctrl = false, bool shift = false) {
        if (i < 0) {
            if (!ctrl)
                selection.clear();
            selected = -1;
            requestPreview();
            draw();
            return;
        }
        if (shift && anchor >= 0) {
            int a = position(anchor), b = position(i);
            if (!ctrl)
                selection.clear();
            if (a >= 0 && b >= 0)
                for (int p = std::min(a, b); p <= std::max(a, b); p++)
                    selection.insert(visible[p]);
        } else if (ctrl) {
            if (selection.count(i))
                selection.erase(i);
            else
                selection.insert(i);
            anchor = i;
        } else {
            selection = {i};
            anchor = i;
        }
        selected = i;
        requestPreview();
        draw();
    }
    std::vector<std::wstring> selectedPaths() const {
        std::vector<std::wstring> p;
        for (auto i : selection)
            if (i >= 0 && i < (int)listing->entries.size())
                p.push_back(listing->entries[i].path);
        return p;
    }
    void openSelected(bool newWindow = false) {
        if (path == L"::recycle") {
            recycleVerb("properties", L"properties");
            return;
        }
        if (selected < 0 || selected >= (int)listing->entries.size())
            return;
        auto e = listing->entries[selected];
        if (e.folder || lower(fs::path(e.path).extension().wstring()) == L".zip") {
            if (newWindow)
                openFileBrowser(e.path, false, true);
            else
                load(e.path);
        } else if (shellPath(path) && GetFileAttributesW(e.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            GUID guid{};
            CoCreateGuid(&guid);
            wchar_t value[64];
            StringFromGUID2(guid, value, 64);
            auto folder = fs::path(stateDirectory()) / L"file-open" / value;
            std::error_code ec;
            fs::create_directories(folder, ec);
            if (ec) {
                status = files::errorText(HRESULT_FROM_WIN32(ec.value()));
                draw();
                return;
            }
            pendingOpen = (folder / e.name).wstring();
            start({files::Operation::Copy, {e.path}, folder.wstring(), e.name});
        } else
            openTarget(e.path);
    }
    void start(files::Job job) {
        if (busy()) {
            status = L"Дождитесь завершения операции";
            draw();
            return;
        }
        if (job.kind == files::Operation::Copy || job.kind == files::Operation::Move) {
            if (job.targets.empty() && !files::absolutePath(job.destination)) {
                status = L"Выберите папку назначения";
                draw();
                return;
            }
            for (auto &p : job.sources) {
                auto src = lower(fs::path(p).lexically_normal().wstring()),
                     dst = lower(fs::path(job.destination).lexically_normal().wstring());
                if (dst == src || dst.rfind(src + L"\\", 0) == 0) {
                    status = L"Нельзя поместить папку внутрь самой себя";
                    draw();
                    return;
                }
            }
        }
        if (operations.start(hwnd, std::move(job))) {
            operationPending = true;
            status.clear();
            draw();
        }
    }
    void copy(bool cut) {
        auto p = selectedPaths();
        if (!p.empty() && !files::clipboard(hwnd, p, cut)) {
            status = L"Буфер обмена недоступен";
            draw();
        }
    }
    void paste() {
        DWORD effect;
        auto p = files::clipboardPaths(hwnd, effect);
        if (!p.empty()) {
            if (effect & DROPEFFECT_MOVE) {
                moveClipboardSequence = GetClipboardSequenceNumber();
                clipboardMoveSources = p;
            }
            start({effect & DROPEFFECT_MOVE ? files::Operation::Move : files::Operation::Copy, p, path, {}});
        }
    }
    void erase(bool permanent = false) {
        if (path == L"::recycle") {
            recycleVerb("delete", L"delete");
            return;
        }
        auto p = selectedPaths();
        if (p.empty())
            return;
        if (permanent &&
            MessageBoxW(hwnd, (L"Удалить без возможности восстановления: " + std::to_wstring(p.size()) + L"?").c_str(),
                        L"Удаление", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            return;
        start({permanent ? files::Operation::Delete : files::Operation::Recycle, p, {}, {}});
    }
    void rename() {
        if (path == L"::recycle" || busy() || selection.empty() || selected < 0)
            return;
        int p = position(selected);
        if (p < 0)
            return;
        ensureVisible();
        auto e = listing->entries[selected];
        auto b = itemBox(p);
        SetWindowTextW(renameEdit, selection.size() > 1 ? fs::path(e.name).stem().c_str() : e.name.c_str());
        SetWindowPos(renameEdit, nullptr, (int)((grid ? b.x + 3 : b.x + 42) * scale),
                     (int)((grid ? b.y + 74 : b.y + 3) * scale),
                     (int)((grid ? b.w - 6 : std::max(180.f, b.w - 210)) * scale), (int)(28 * scale),
                     SWP_NOZORDER | SWP_SHOWWINDOW);
        SetFocus(renameEdit);
        auto dot = e.name.find_last_of(L'.');
        SendMessageW(renameEdit, EM_SETSEL, 0, e.folder || dot == std::wstring::npos ? -1 : (LPARAM)dot);
    }
    void commitRename() {
        auto name = text(renameEdit);
        if (!files::validName(name)) {
            status = L"Недопустимое имя файла";
            draw();
            SetFocus(renameEdit);
            return;
        }
        ShowWindow(renameEdit, SW_HIDE);
        if (selected < 0 || selected >= (int)listing->entries.size())
            return;
        auto e = listing->entries[selected];
        if (selection.size() == 1) {
            if (e.name == name)
                return;
            selectName = name;
            start({files::Operation::Rename, {e.path}, {}, name});
        } else {
            files::Job job;
            job.kind = files::Operation::Rename;
            int n = 1;
            for (auto i : visible)
                if (selection.count(i)) {
                    auto &entry = listing->entries[i];
                    auto target = fs::path(entry.path).parent_path() /
                                  (name + L" (" + std::to_wstring(n++) + L")" +
                                   (entry.folder ? L"" : fs::path(entry.name).extension().wstring()));
                    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        status = L"Имя уже занято";
                        draw();
                        return;
                    }
                    job.sources.push_back(entry.path);
                    job.targets.push_back(target.wstring());
                }
            start(std::move(job));
        }
    }
    void newItem(bool folder) {
        if (!files::absolutePath(path))
            return;
        std::wstring name = folder ? L"Новая папка" : L"Новый текстовый документ.txt";
        for (int n = 2; GetFileAttributesW((fs::path(path) / name).c_str()) != INVALID_FILE_ATTRIBUTES && n < 10000;
             n++)
            name = folder ? L"Новая папка (" + std::to_wstring(n) + L")"
                          : L"Новый текстовый документ (" + std::to_wstring(n) + L").txt";
        selectName = name;
        start({folder ? files::Operation::NewFolder : files::Operation::NewText, {}, path, name});
    }
    void undoLast(bool forward = false) {
        auto &stack = forward ? redo : undo;
        if (busy() || stack.empty())
            return;
        auto changes = stack.back();
        files::Job job;
        bool removal =
            changes.front().kind == files::Operation::Copy || changes.front().kind == files::Operation::NewFolder ||
            changes.front().kind == files::Operation::NewText || changes.front().kind == files::Operation::Zip ||
            changes.front().kind == files::Operation::Extract || changes.front().kind == files::Operation::Link;
        job.kind = removal ? files::Operation::Recycle : files::Operation::Move;
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            auto &c = *it;
            if (!c.undoable || c.result.empty())
                return;
            if (!removal) {
                if (c.source.empty())
                    return;
                if (GetFileAttributesW(c.source.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    status = L"Исходное имя уже занято";
                    draw();
                    return;
                }
                job.targets.push_back(c.source);
            }
            job.sources.push_back(c.result);
        }
        pendingUndo = changes;
        stack.pop_back();
        undoDirection = forward ? -1 : 1;
        start(std::move(job));
    }
    void recycleVerb(const char *verb, const wchar_t *wide) {
        health::Hold protection;
        if (!protection) return;
        if (path != L"::recycle" || selection.empty() || busy())
            return;
        std::vector<PCUITEMID_CHILD> children;
        Com<IShellFolder> parent;
        for (auto i : selection) {
            auto &e = listing->entries[i];
            if (e.shellId.empty())
                continue;
            auto pidl = (PCIDLIST_ABSOLUTE)e.shellId.data();
            if (!parent) {
                PCUITEMID_CHILD last = nullptr;
                if (FAILED(SHBindToParent(pidl, IID_PPV_ARGS(parent.put()), &last)))
                    return;
            }
            children.push_back(ILFindLastID(pidl));
        }
        if (children.empty() || !parent)
            return;
        Com<IContextMenu> menu;
        HRESULT hr = parent->GetUIObjectOf(hwnd, (UINT)children.size(), children.data(), IID_IContextMenu, nullptr,
                                           (void **)menu.put());
        if (SUCCEEDED(hr)) {
            HMENU temporary = CreatePopupMenu();
            menu->QueryContextMenu(temporary, 0, 1, 0x7fff, CMF_NORMAL);
            CMINVOKECOMMANDINFOEX info{};
            info.cbSize = sizeof(info);
            info.fMask = CMIC_MASK_UNICODE;
            info.hwnd = hwnd;
            info.lpVerb = verb;
            info.lpVerbW = wide;
            info.nShow = SW_SHOWNORMAL;
            operationPending = true;
            hr = menu->InvokeCommand((CMINVOKECOMMANDINFO *)&info);
            operationPending = false;
            DestroyMenu(temporary);
        }
        if (FAILED(hr))
            status = files::errorText(hr);
        refresh();
        SetTimer(hwnd, 8, 1200, nullptr);
    }
    void restoreRecycle() { recycleVerb("undelete", L"undelete"); }
    void archive(bool extract) {
        auto p = selectedPaths();
        if (p.empty() || !files::absolutePath(path))
            return;
        auto base = fs::path(p[0]).stem().wstring();
        if (base.empty())
            base = L"Архив";
        auto name = base + (extract ? L"" : L".zip");
        for (int i = 2; GetFileAttributesW((fs::path(path) / name).c_str()) != INVALID_FILE_ATTRIBUTES && i < 10000;
             i++)
            name = base + L" (" + std::to_wstring(i) + L")" + (extract ? L"" : L".zip");
        selectName = name;
        start({extract ? files::Operation::Extract : files::Operation::Zip, p, path, name});
    }
    void viewMenu(POINT p) {
        int n = customMenu(hwnd, p,
                           {{L"Таблица", nullptr, 1, !grid},
                            {L"Крупные значки", nullptr, 2, grid},
                            {L"Скрытые элементы", nullptr, 3, showHidden},
                            {L"Расширения файлов", nullptr, 4, showExtensions},
                            {L"Миниатюры", nullptr, 5, thumbnail},
                            {L"Область просмотра", nullptr, 6, previewEnabled}},
                           palette);
        if (n == 1 || n == 2) {
            grid = n == 2;
            scroll = 0;
        }
        if (n == 3)
            showHidden = !showHidden;
        if (n == 4)
            showExtensions = !showExtensions;
        if (n == 5)
            thumbnail = !thumbnail;
        if (n == 6) {
            previewEnabled = !previewEnabled;
            requestPreview();
        }
        savePreferences();
        if (n == 3 || n == 5 || n == 2)
            refresh();
        else
            draw();
    }
    void action(int id) {
        if (id >= 1000 && id < 1012) {
            tab(id - 1000);
            return;
        }
        if (id >= 1100 && id < 1112) {
            closeTab(id - 1100);
            return;
        }
        if (id >= 10000 && id < 14096) {
            int i = id - 10000;
            if (i < (int)sidebar.size())
                load(sidebar[i].second);
            return;
        }
        if (id >= 20000 && id < 24096) {
            treeToggle(id - 20000);
            return;
        }
        if (id >= 30000 && id < 40000) {
            int i = id - 30000;
            if (i < (int)crumbs.size())
                load(crumbs[i].second);
            return;
        }
        if (id >= 3000 && id < 3004) {
            if (sortColumn == id - 3000)
                descending = !descending;
            else {
                sortColumn = id - 3000;
                descending = false;
            }
            rebuild();ensureVisible();
            savePreferences();
            draw();
            return;
        }
        switch (id) {
        case 64:
            columnWidth[0] = 0;
            columnWidth[1] = 154;
            columnWidth[2] = 100;
            columnWidth[3] = 104;
            savePreferences();
            draw();
            break;
        case 50:
            beginAddress();
            break;
        case 51: {
            std::vector<MenuItem> menu;
            for (int i = 0; i < (int)crumbs.size() && i < 20; i++)
                menu.push_back({crumbs[i].first, nullptr, i + 1});
            POINT p{};
            GetCursorPos(&p);
            int n = customMenu(hwnd, p, menu, palette);
            if (n > 0)
                load(crumbs[n - 1].second);
            break;
        }
        case 65: {
            POINT p{};
            GetCursorPos(&p);
            columnsMenu(p);
            break;
        }

        case 30:
            grid = true;
            scroll = 0;
            refresh();
            break;
        case 31:
            grid = false;
            scroll = 0;
            draw();
            break;
        case 32:
            showHidden = !showHidden;
            refresh();
            break;
        case 44:
            restoreRecycle();
            break;
        case 40:
            archive(false);
            break;
        case 41:
            archive(true);
            break;
        case 42:
            newItem(false);
            break;
        case 43:
            undoLast(true);
            break;
        case 1:
            if (historyAt > 0)
                load(history[--historyAt], false);
            break;
        case 10:
            if (historyAt + 1 < (int)history.size())
                load(history[++historyAt], false);
            break;
        case 2: {
            auto p = fs::path(path).parent_path();
            if (path.rfind(L"::", 0) == 0 || p.empty() || p == fs::path(path))
                load(L"::computer");
            else
                load(p.wstring());
            break;
        }
        case 3:
            load(home());
            break;
        case 4:
            newItem(true);
            break;
        case 5:
            copy(false);
            break;
        case 6:
            paste();
            break;
        case 7:
            refresh();
            break;
        case 8:
            erase();
            break;
        case 11:
            newTab(path);
            break;
        case 12: {
            POINT p{};
            GetCursorPos(&p);
            viewMenu(p);
            break;
        }
        case 13: {
            POINT p{};
            GetCursorPos(&p);
            context(p);
            break;
        }
        case 20:
            undoLast();
            break;
        case 21:
            operations.cancel();
            break;
        case 101:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case 102:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case 103:
            if (!busy())
                DestroyWindow(hwnd);
            else {
                status = L"Сначала завершите или отмените операцию";
                draw();
            }
            break;
        }
    }
    void shellActions(POINT point, bool background = false, bool create = false) {
        health::Hold protection;if(!protection)return;
        auto paths = selectedPaths();
        if (busy())
            return;
        if (background)
            paths = {path};
        if (paths.empty())
            return;
        std::vector<PIDLIST_ABSOLUTE> ids;
        Com<IShellFolder> parent;
        PIDLIST_ABSOLUTE common = nullptr;
        HRESULT hr = S_OK;
        for (auto &path : paths) {
            PIDLIST_ABSOLUTE id = nullptr;
            hr = SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr);
            if (FAILED(hr))
                break;
            auto folder = ILCloneFull(id);
            ILRemoveLastID(folder);
            if (common && !ILIsEqual(common, folder)) {
                CoTaskMemFree(folder);
                CoTaskMemFree(id);
                hr = E_INVALIDARG;
                break;
            }
            if (!common)
                common = folder;
            else
                CoTaskMemFree(folder);
            ids.push_back(id);
        }
        Com<IContextMenu> context;
        Com<IContextMenu2> context2;
        if (SUCCEEDED(hr) && !ids.empty()) {
            PCUITEMID_CHILD last = nullptr;
            hr = SHBindToParent(ids[0], IID_PPV_ARGS(parent.put()), &last);
            std::vector<PCUITEMID_CHILD> children;
            for (auto id : ids)
                children.push_back(ILFindLastID(id));
            if (SUCCEEDED(hr)) {
                if (background) {
                    Com<IShellItem> folder;
                    hr = SHCreateItemFromIDList(ids[0], IID_PPV_ARGS(folder.put()));
                    if (SUCCEEDED(hr))
                        hr = folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(parent.put()));
                    if (SUCCEEDED(hr))
                        hr = parent->CreateViewObject(hwnd, IID_IContextMenu, (void **)context.put());
                } else
                    hr = parent->GetUIObjectOf(hwnd, (UINT)children.size(), children.data(), IID_IContextMenu, nullptr,
                                               (void **)context.put());
            }
        }
        if (SUCCEEDED(hr) && context) {
            operationPending = true;
            HMENU root = CreatePopupMenu();
            hr = context->QueryContextMenu(root, 0, 1, 0x6fff, CMF_NORMAL | CMF_EXTENDEDVERBS);
            context->QueryInterface(IID_PPV_ARGS(context2.put()));
            std::vector<HMENU> chain{root};
            unsigned chosen = 0;
            int page = 0;
            if (create) {
                bool found = false;
                for (int i = 0; i < GetMenuItemCount(root); i++) {
                    wchar_t label[256] = {};
                    GetMenuStringW(root, i, label, 256, MF_BYPOSITION);
                    std::wstring name = lower(label);
                    std::erase(name, L'&');
                    HMENU child = GetSubMenu(root, i);
                    if (child && (name == L"создать" || name == L"new")) {
                        if (context2)
                            context2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)child, MAKELPARAM(i, FALSE));
                        chain = {child};
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    hr = E_NOTIMPL;
                }
            }
            while (SUCCEEDED(hr) && !chain.empty()) {
                HMENU current = chain.back();
                std::vector<std::pair<std::wstring, int>> entries;
                for (int i = 0; i < GetMenuItemCount(current); i++) {
                    MENUITEMINFOW item{};
                    item.cbSize = sizeof(item);
                    item.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
                    wchar_t title[1024] = {};
                    item.dwTypeData = title;
                    item.cch = 1023;
                    if (!GetMenuItemInfoW(current, i, TRUE, &item) || (item.fType & MFT_SEPARATOR) ||
                        (item.fState & (MFS_DISABLED | MFS_GRAYED)))
                        continue;
                    std::wstring label = title;
                    if (auto tab = label.find(L'\t'); tab != std::wstring::npos)
                        label.resize(tab);
                    std::wstring clean;
                    for (size_t n = 0; n < label.size(); n++) {
                        if (label[n] == L'&') {
                            if (n + 1 < label.size() && label[n + 1] == L'&')
                                ++n;
                            else
                                continue;
                        }
                        clean += label[n];
                    }
                    if (clean.empty())
                        continue;
                    if (item.hSubMenu)
                        clean += L"  ›";
                    entries.push_back({clean, i});
                }
                std::vector<MenuItem> menu;
                if (chain.size() > 1)
                    menu.push_back({L"‹ Назад", nullptr, 9001});
                int start = page * 17;
                for (int i = start; i < (int)entries.size() && i < start + 17; i++)
                    menu.push_back({entries[i].first, nullptr, i + 1});
                if (page > 0)
                    menu.push_back({L"Предыдущие", nullptr, 9002});
                if (start + 17 < (int)entries.size())
                    menu.push_back({L"Следующие", nullptr, 9003});
                int picked = customMenu(hwnd, point, menu, palette);
                if (picked == 9001) {
                    chain.pop_back();
                    page = 0;
                    continue;
                }
                if (picked == 9002) {
                    --page;
                    continue;
                }
                if (picked == 9003) {
                    ++page;
                    continue;
                }
                if (picked <= 0 || picked > (int)entries.size())
                    break;
                int index = entries[picked - 1].second;
                HMENU child = GetSubMenu(current, index);
                if (child) {
                    if (context2)
                        context2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)child, MAKELPARAM(index, FALSE));
                    chain.push_back(child);
                    page = 0;
                    continue;
                }
                UINT id = GetMenuItemID(current, index);
                if (id >= 1 && id < 0x7000)
                    chosen = id;
                break;
            }
            if (chosen) {
                wchar_t verb[256] = {};
                context->GetCommandString(chosen - 1, GCS_VERBW, nullptr, (LPSTR)verb, 256);
                std::wstring action = lower(verb);
                if (!background && selected >= 0 && listing->entries[selected].folder &&
                    (action == L"open" || action == L"explore" || action == L"opennewwindow" ||
                     action == L"opennewtab" || action == L"opennewprocess")) {
                    operationPending = false;
                    if (action == L"opennewtab")
                        newTab(paths.front());
                    else
                        openSelected(action != L"open" && action != L"explore");
                    operationPending = true;
                } else {
                    CMINVOKECOMMANDINFOEX info{};
                    info.cbSize = sizeof(info);
                    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                    info.hwnd = hwnd;
                    info.lpVerb = MAKEINTRESOURCEA(chosen - 1);
                    info.lpVerbW = MAKEINTRESOURCEW(chosen - 1);
                    info.ptInvoke = point;
                    info.nShow = SW_SHOWNORMAL;
                    hr = context->InvokeCommand((CMINVOKECOMMANDINFO *)&info);
                }
            }
            DestroyMenu(root);
            operationPending = false;
            refresh();
        }
        for (auto id : ids)
            CoTaskMemFree(id);
        CoTaskMemFree(common);
        if (FAILED(hr)) {
            status =
                paths.size() > 1 && hr == E_INVALIDARG ? L"Выберите элементы из одной папки" : files::errorText(hr);
            draw();
        }
    }
    void context(POINT p) {
        std::vector<MenuItem> items;
        auto paths = selectedPaths();
        if (!paths.empty()) {
            items = {{L"Открыть", L"\uE8A5", 1},
                     {L"Открыть в новой вкладке", nullptr, 10},
                     {L"Открыть в новом окне", nullptr, 11},
                     {L"Открыть с помощью…", nullptr, 12},
                     {L"Копировать", L"\uE8C8", 2},
                     {L"Вырезать", L"\uE8C6", 3},
                     {L"Переименовать", nullptr, 4},
                     {path == L"::recycle" ? L"Удалить навсегда" : L"В корзину", L"\uE74D", 5},
                     {L"Копировать путь", nullptr, 6},
                     {L"Свойства", nullptr, 13}};
            if (path == L"::recycle")
                items = {
                    {L"Восстановить", nullptr, 18}, {L"Удалить навсегда", L"\uE74D", 5}, {L"Свойства", nullptr, 13}};
            else {
                items.push_back({L"Сжать в ZIP", nullptr, 14});
                items.push_back({L"Дополнительные действия…", nullptr, 25});
                if (paths.size() == 1 && lower(fs::path(paths[0]).extension().wstring()) == L".zip")
                    items.push_back({L"Извлечь ZIP", nullptr, 15});
                items.push_back({L"Создать ярлык", nullptr, 16});
                if (paths.size() == 1) {
                    auto ext = lower(fs::path(paths[0]).extension().wstring());
                    if (ext == L".exe" || ext == L".lnk" || ext == L".bat" || ext == L".cmd" || ext == L".msc")
                        items.push_back({L"От имени администратора", nullptr, 17});
                    if (selected >= 0 && listing->entries[selected].folder)
                        items.push_back({L"Закрепить папку", nullptr, 19});
                }
            }
        } else {
            items = {{L"Создать…", nullptr, 26},
                     {L"Действия папки…", nullptr, 25},
                     {L"Вставить", L"\uE77F", 7},
                     {L"Новая папка", L"\uE8F4", 8},
                     {L"Текстовый документ", nullptr, 20},
                     {L"Обновить", L"\uE72C", 9},
                     {L"Вид", nullptr, 21},
                     {L"Закрепить папку", nullptr, 19},
                     {L"Свойства папки", nullptr, 22},
                     {L"Открыть терминал", nullptr, 23}};
            if (path == L"::recycle")
                items = {{L"Обновить", nullptr, 9}, {L"Очистить корзину", nullptr, 24}};
        }
        if (path != L"::recycle" && !paths.empty()) {
            bool folder =
                paths.size() == 1 && selected >= 0 &&
                (listing->entries[selected].folder || lower(fs::path(paths[0]).extension().wstring()) == L".zip");
            std::erase_if(items, [&](const MenuItem &item) {
                return ((item.id == 10 || item.id == 11) && !folder) ||
                       (item.id == 12 && (paths.size() != 1 || folder));
            });
        }
        int r = customMenu(hwnd, p, items, palette);
        if (r == 25)
            shellActions(p, paths.empty());
        if (r == 26)
            shellActions(p, true, true);
        if (r == 1)
            openSelected();
        if (r == 2)
            copy(false);
        if (r == 3)
            copy(true);
        if (r == 4)
            rename();
        if (r == 5)
            erase(path == L"::recycle");
        if (r == 6) {
            std::wstring s;
            for (auto &path : paths) {
                if (!s.empty())
                    s += L"\r\n";
                s += path;
            }
            setClipboard(hwnd, s);
        }
        if (r == 7)
            paste();
        if (r == 8)
            newItem(true);
        if (r == 9)
            refresh();
        if (r == 10 && selected >= 0 && listing->entries[selected].folder)
            newTab(paths.front());
        if (r == 11)
            openSelected(true);
        if (r == 12 && paths.size() == 1)
            files::openWith(hwnd, paths.front());
        if (r == 13) {
            if (path == L"::recycle")
                recycleVerb("properties", L"properties");
            else
                files::properties(hwnd, paths);
        }
        if (r == 14)
            archive(false);
        if (r == 15)
            archive(true);
        if (r == 16)
            start({files::Operation::Link, paths, path, {}});
        if (r == 17 && !paths.empty()) {
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.hwnd = hwnd;
            info.lpVerb = L"runas";
            info.lpFile = paths[0].c_str();
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info) && GetLastError() != ERROR_CANCELLED) {
                status = files::errorText(HRESULT_FROM_WIN32(GetLastError()));
                draw();
            }
        }
        if (r == 18)
            restoreRecycle();
        if (r == 19) {
            auto folder = paths.empty() ? path : paths.front();
            if (files::absolutePath(folder) &&
                std::find(favorites.begin(), favorites.end(), folder) == favorites.end() && favorites.size() < 16) {
                favorites.push_back(folder);
                saveFavorites();
                draw();
            }
        }
        if (r == 20)
            newItem(false);
        if (r == 21)
            viewMenu(p);
        if (r == 22)
            files::properties(hwnd, {path});
        if (r == 23 && files::absolutePath(path))
            ShellExecuteW(hwnd, L"open", L"powershell.exe", L"-NoLogo -NoExit", path.c_str(), SW_SHOWNORMAL);
        if (r == 24) {
            SHEmptyRecycleBinW(hwnd, nullptr, 0);
            refresh();
        }
    }
    void drag() {
        auto p = selectedPaths();
        if (p.empty())
            return;
        Com<IDataObject> data;
        if (FAILED(files::dataObject(p, data.put())))
            return;
        DWORD effect = 0;
        auto handler = drop;
        auto window = hwnd;
        handler->AddRef();
        DoDragDrop(data.p, handler, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        handler->Release();
        if (IsWindow(window) && GetWindowLongPtrW(window, GWLP_USERDATA) == (LONG_PTR)this)
            refresh();
    }
    void key(WPARAM a) {
        if (a == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            if (GetKeyState(VK_SHIFT) & 0x8000)
                SetFocus(searchEdit);
            else
                beginAddress();
            return;
        }
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'L') {
            beginAddress();
            SendMessageW(pathEdit, EM_SETSEL, 0, -1);
        } else if (ctrl && a == 'P') {
            previewEnabled = !previewEnabled;
            requestPreview();
            draw();
        } else if ((ctrl && (a == 'F' || a == 'E')) || a == VK_F3) {
            SetFocus(searchEdit);
            SendMessageW(searchEdit, EM_SETSEL, 0, -1);
        } else if (ctrl && a == 'A') {
            selection.clear();
            for (auto i : visible)
                selection.insert(i);
            if (selected < 0 && !visible.empty())
                selected = visible[0];
            draw();
        } else if (ctrl && a == 'C')
            copy(false);
        else if (ctrl && a == 'X')
            copy(true);
        else if (ctrl && a == 'V')
            paste();
        else if (ctrl && a == 'Z')
            undoLast(shift);
        else if (ctrl && a == 'Y')
            undoLast(true);
        else if (ctrl && a == 'T')
            newTab(path);
        else if (ctrl && shift && a == 'N')
            newItem(true);
        else if (ctrl && a == 'N')
            openFileBrowser(path, false, true);
        else if (ctrl && a == 'W')
            closeTab(tabAt);
        else if (ctrl && a == VK_TAB)
            tab((tabAt + (shift ? (int)tabs.size() - 1 : 1)) % (int)tabs.size());
        else if (a == VK_F2)
            rename();
        else if (a == VK_F5)
            refresh();
        else if (a == VK_DELETE)
            erase(shift || path == L"::recycle");
        else if (a == VK_RETURN)
            openSelected();
        else if (a == VK_BACK)
            action(2);
        else if (a == VK_ESCAPE) {
            query.clear();
            SetWindowTextW(searchEdit, L"");
            refresh();
        } else if (a == VK_UP || a == VK_DOWN || a == VK_LEFT || a == VK_RIGHT || a == VK_HOME || a == VK_END ||
                   a == VK_PRIOR || a == VK_NEXT) {
            if (visible.empty())
                return;
            int p = position(selected);
            if (a == VK_HOME)
                p = 0;
            else if (a == VK_END)
                p = (int)visible.size() - 1;
            else if (p < 0)
                p = 0;
            else
                p += a == VK_UP      ? -columns()
                     : a == VK_DOWN  ? columns()
                     : a == VK_LEFT  ? -1
                     : a == VK_RIGHT ? 1
                     : a == VK_PRIOR ? -capacity()
                                     : capacity();
            p = std::clamp(p, 0, (int)visible.size() - 1);
            if (ctrl && !shift)
                selected = visible[p];
            else
                choose(visible[p], ctrl, shift);
            ensureVisible();
            draw();
        }
    }
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
};
HRESULT DropHandler::DragEnter(IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) {
    FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    acceptable = data && SUCCEEDED(data->QueryGetData(&f));
    if (data && !acceptable) {
        f.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        acceptable = SUCCEEDED(data->QueryGetData(&f));
    }
    allowed = *effect;
    return DragOver(keys, pt, effect);
}
HRESULT DropHandler::DragOver(DWORD keys, POINTL pt, DWORD *effect) {
    POINT p{pt.x, pt.y};
    ScreenToClient(browser->hwnd, &p);
    browser->dropIndex = browser->rowAt(p.x, p.y);
    bool folder = browser->dropIndex >= 0 && browser->listing->entries[browser->dropIndex].folder;
    bool valid = files::absolutePath(browser->path) || folder;
    DWORD desired = (keys & MK_CONTROL) && (keys & MK_SHIFT) ? DROPEFFECT_LINK
                    : keys & MK_SHIFT                        ? DROPEFFECT_MOVE
                                                             : DROPEFFECT_COPY;
    *effect = acceptable && valid && !browser->busy() ? (allowed & desired) : DROPEFFECT_NONE;
    browser->draw();
    return S_OK;
}
HRESULT DropHandler::DragLeave() {
    browser->dropIndex = -1;
    browser->draw();
    return S_OK;
}
HRESULT DropHandler::Drop(IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) {
    DragOver(keys, pt, effect);
    DWORD desired = *effect;
    HWND hwnd = browser->hwnd;
    if (desired) {
        auto p = files::dropPaths(data);
        auto dest = browser->path;
        if (browser->dropIndex >= 0 && browser->listing->entries[browser->dropIndex].folder)
            dest = browser->listing->entries[browser->dropIndex].path;
        if (p.empty()) {
            *effect = DROPEFFECT_NONE;
            return DragLeave();
        }
        browser->start({desired == DROPEFFECT_MOVE   ? files::Operation::Move
                        : desired == DROPEFFECT_LINK ? files::Operation::Link
                                                     : files::Operation::Copy,
                        p,
                        dest,
                        {}});
        if (!browser->busy()) {
            *effect = DROPEFFECT_NONE;
            return DragLeave();
        }
        // Do not report MOVE before it has finished: non-async OLE sources may delete
        // their originals when Drop returns. The nested pump keeps the window responsive.
        while (IsWindow(hwnd) && GetWindowLongPtrW(hwnd, GWLP_USERDATA) == (LONG_PTR)browser && browser->busy()) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage((int)message.wParam);
                    *effect = DROPEFFECT_NONE;
                    return S_OK;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(5);
        }
        if (!IsWindow(hwnd) || GetWindowLongPtrW(hwnd, GWLP_USERDATA) != (LONG_PTR)browser) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        auto result = browser->operations.snapshot();
        if (result.cancelled || FAILED(result.error))
            *effect = DROPEFFECT_NONE;
    }
    return DragLeave();
}
LRESULT CALLBACK Browser::editProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto self = (Browser *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_KEYDOWN && a == VK_TAB && w != self->renameEdit && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        HWND target =
            w == self->pathEdit ? (reverse ? self->hwnd : self->searchEdit) : (reverse ? self->pathEdit : self->hwnd);
        if (target == self->pathEdit)
            self->beginAddress();
        else
            SetFocus(target);
        return 0;
    }
    if (w == self->pathEdit && m == WM_KILLFOCUS) {
        self->addressEditing = false;
        ShowWindow(w, SW_HIDE);
        self->draw();
    }
    if(m==WM_KEYDOWN){bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;if((ctrl&&(a=='T'||a=='N'||a=='W'||a==VK_TAB||a=='L'||a=='F'||a=='E'))||a==VK_F3||a==VK_F5){self->key(a);return 0;}}
    if(m==WM_SYSKEYDOWN&&(a=='D'||a=='P'||a==VK_LEFT||a==VK_RIGHT||a==VK_UP)){SendMessageW(self->hwnd,m,a,b);return 0;}
    if (m == WM_KEYDOWN && a == VK_RETURN) {
        if (w == self->pathEdit)
            self->load(text(w));
        else if (w == self->searchEdit) {
            self->query = text(w);
            self->scroll = 0;
            self->selection.clear();
            self->requestListing();
        } else
            self->commitRename();
        SetFocus(self->hwnd);
        return 0;
    }
    if (m == WM_KEYDOWN && a == VK_ESCAPE) {
        if (w == self->renameEdit)
            ShowWindow(w, SW_HIDE);
        else if (w == self->searchEdit) {
            SetWindowTextW(w, L"");
            self->query.clear();
            self->refresh();
        } else
            SetWindowTextW(w, self->path.c_str());
        SetFocus(self->hwnd);
        return 0;
    }
    if (w == self->searchEdit && m == WM_PAINT) {
        LRESULT result = CallWindowProcW(self->oldSearch, w, m, a, b);
        if (GetWindowTextLengthW(w) == 0 && GetFocus() != w) {
            HDC dc = GetDC(w);
            RECT r{};
            GetClientRect(w, &r);
            auto oldFont = SelectObject(dc, self->font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB((palette.muted >> 16) & 255, (palette.muted >> 8) & 255, palette.muted & 255));
            DrawTextW(dc, L"Поиск в папке", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            ReleaseDC(w, dc);
        }
        return result;
    }
    if (w == self->searchEdit && (m == WM_SETFOCUS || m == WM_KILLFOCUS))
        InvalidateRect(w, nullptr, TRUE);
    return CallWindowProcW(w == self->pathEdit     ? self->oldPath
                           : w == self->renameEdit ? self->oldRename
                                                   : self->oldSearch,
                           w, m, a, b);
}
LRESULT CALLBACK Browser::proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto self = (Browser *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        self = (Browser *)((CREATESTRUCTW *)b)->lpCreateParams;
        self->hwnd = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self)
        return DefWindowProcW(w, m, a, b);
    switch (m) {
    case WM_CREATE: {
        self->scale = GetDpiForWindow(w) / 96.f;
        self->editBrush =
            CreateSolidBrush(RGB((palette.card >> 16) & 255, (palette.card >> 8) & 255, palette.card & 255));
        self->initPreferences();
        self->tabs.push_back({});
        self->pathEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 1, 1, w,
                                         (HMENU)201, GetModuleHandleW(nullptr), nullptr);
        self->renameEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1, w, (HMENU)202,
                                           GetModuleHandleW(nullptr), nullptr);
        self->searchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 1, 1, w,
                                           (HMENU)203, GetModuleHandleW(nullptr), nullptr);
        for (HWND edit : {self->pathEdit, self->renameEdit, self->searchEdit}) {
            SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)self);
            SendMessageW(edit, EM_SETLIMITTEXT, 32767, 0);
        }
        self->fonts();
        self->oldPath = (WNDPROC)SetWindowLongPtrW(self->pathEdit, GWLP_WNDPROC, (LONG_PTR)editProc);
        self->oldRename = (WNDPROC)SetWindowLongPtrW(self->renameEdit, GWLP_WNDPROC, (LONG_PTR)editProc);
        self->oldSearch = (WNDPROC)SetWindowLongPtrW(self->searchEdit, GWLP_WNDPROC, (LONG_PTR)editProc);
        SendMessageW(self->searchEdit, 0x1501, TRUE, (LPARAM)L"Поиск в папке");
        self->worker = std::thread([self] { self->run(); });
        self->drop = new DropHandler(self);
        RegisterDragDrop(w, self->drop);
        SetTimer(w, 6, 200, nullptr);
        self->resize();
        return 0;
    }
    case WM_NCCALCSIZE:
        if (a)
            return 0;
        break;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        ScreenToClient(w, &p);
        RECT r{};
        GetClientRect(w, &r);
        int border = (int)(6 * self->scale);
        if (!IsZoomed(w)) {
            if (p.y < border)
                return p.x < border ? HTTOPLEFT : p.x > r.right - border ? HTTOPRIGHT : HTTOP;
            if (p.y > r.bottom - border)
                return p.x < border ? HTBOTTOMLEFT : p.x > r.right - border ? HTBOTTOMRIGHT : HTBOTTOM;
            if (p.x < border)
                return HTLEFT;
            if (p.x > r.right - border)
                return HTRIGHT;
        }
        if (p.y < 45 * self->scale) {
            bool on = false;
            for (auto h : self->hits)
                if (h.rect.contains(p.x / self->scale, p.y / self->scale)) {
                    on = true;
                    break;
                }
            if (!on)
                return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)b)->ptMinTrackSize = {(LONG)(720 * self->scale), (LONG)(450 * self->scale)};
        return 0;
    case WM_SIZE:
        if (a != SIZE_MINIMIZED)
            self->resize();
        return 0;
    case WM_DPICHANGED: {
        auto r = (RECT *)b;
        SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        self->resize();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT p;
        BeginPaint(w, &p);
        EndPaint(w, &p);
        self->draw();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_APP + 1: {
        std::shared_ptr<const Listing> result;
        {
            std::lock_guard lock(self->mutex);
            result = self->result;
            if (!result || result->revision != self->revision)
                return 0;
        }
        std::set<std::wstring> old = self->preserve;
        if (self->listing->path == result->path)
            for (auto i : self->selection)
                if (i >= 0 && i < (int)self->listing->entries.size())
                    old.insert(self->listing->entries[i].path);
        self->listing = result;
        self->visuals.clear();
        self->iconQueued.clear();
        self->visualRevision = result->revision;
        self->loading = false;
        self->selection.clear();
        self->selected = -1;
        for (int i = 0; i < (int)result->entries.size(); i++) {
            auto &e = result->entries[i];
            if (self->selectName.empty() ? old.count(e.path) != 0
                                         : !_wcsicmp(e.name.c_str(), self->selectName.c_str())) {
                self->selection.insert(i);
                self->selected = i;
            }
        }
        self->preserve.clear();
        if (self->selected >= 0)
            self->selectName.clear();
        if(self->treeExpanded.count(result->path)){TreeResult tree;tree.path=result->path;for(auto& entry:result->entries)if(entry.folder)tree.children.push_back({entry.name,entry.path});std::sort(tree.children.begin(),tree.children.end(),[](auto& a,auto& b){return StrCmpLogicalW(a.first.c_str(),b.first.c_str())<0;});self->treeCache[tree.path]=std::move(tree);self->sidebarDirty=true;}
        self->rebuild();
        self->ensureVisible();
        self->requestPreview();
        SetPropW(w, L"OwnIsland.Files.Revision", (HANDLE)(UINT_PTR)result->revision);
        SetPropW(w, L"OwnIsland.Files.Count", (HANDLE)(UINT_PTR)result->entries.size());
        self->draw();
        return 0;
    }
    case WM_APP + 6: {
        std::deque<IconBatch> batches;
        {
            std::lock_guard lock(self->mutex);
            batches.swap(self->iconResults);
        }
        for (auto &batch : batches)
            if (batch.revision == self->visualRevision)
                for (auto &[index, picture] : batch.images) {
                    self->visuals[index] = picture;
                    self->iconQueued.erase(index);
                }
        if (self->visuals.size() > 1024) {
            std::set<int> keep;
            for (int p = std::max(0, self->scroll);
                 p < (int)self->visible.size() && p < self->scroll + self->capacity(); p++)
                keep.insert(self->visible[p]);
            for (auto it = self->visuals.begin(); it != self->visuals.end() && self->visuals.size() > 1024;)
                if (!keep.count(it->first))
                    it = self->visuals.erase(it);
                else
                    ++it;
        }
        int visualMax = 0;
        for (auto &item : self->visuals)
            visualMax = std::max(visualMax, item.first + 1);
        SetPropW(w, L"OwnIsland.Files.VisualMax", (HANDLE)(UINT_PTR)visualMax);
        SetPropW(w, L"OwnIsland.Files.Visuals", (HANDLE)(UINT_PTR)self->visuals.size());
        self->draw();
        return 0;
    }
    case WM_APP + 7: {
        std::deque<TreeResult> results;
        {
            std::lock_guard lock(self->mutex);
            results.swap(self->treeResults);
        }
        for (auto &tree : results) {
            self->treeLoading.erase(tree.path);
            self->treeCache[tree.path] = std::move(tree);
        }
        self->sidebarDirty = true;
        self->draw();
        SetPropW(w, L"OwnIsland.Files.TreeRows", (HANDLE)(UINT_PTR)self->sidebar.size());
        return 0;
    }
    case WM_APP + 2:
        self->resize();
        return 0;
    case WM_APP + 4: {
        {
            std::lock_guard lock(self->mutex);
            if (self->previewResult && self->previewResult->path == self->previewRequest)
                self->preview = self->previewResult;
        }
        self->draw();
        return 0;
    }
    case WM_APP + 3: {
        auto progress = self->operations.snapshot();
        if (!progress.busy) {
            self->operationPending = false;
            if (self->moveClipboardSequence) {
                std::vector<std::wstring> remaining;
                for (auto &source : self->clipboardMoveSources) {
                    bool moved = false;
                    for (auto &change : progress.changes)
                        if (change.kind == files::Operation::Move && !_wcsicmp(source.c_str(), change.source.c_str()) &&
                            !change.result.empty())
                            moved = true;
                    if (!moved)
                        remaining.push_back(source);
                }
                if (self->moveClipboardSequence == GetClipboardSequenceNumber()) {
                    if (remaining.empty() && !self->clipboardMoveSources.empty()) {
                        if (OpenClipboard(w)) {
                            EmptyClipboard();
                            CloseClipboard();
                        }
                    } else if (remaining.size() < self->clipboardMoveSources.size())
                        files::clipboard(w, remaining, true);
                }
                self->moveClipboardSequence = 0;
                self->clipboardMoveSources.clear();
            }
            bool undoable = !progress.changes.empty();
            for (auto &c : progress.changes)
                undoable &= c.undoable;
            if (undoable) {
                auto &stack = self->undoDirection == 1 ? self->redo : self->undo;
                stack.push_back(progress.changes);
                if (stack.size() > 32)
                    stack.erase(stack.begin());
            } else if (!progress.changes.empty()) {
                self->undo.clear();
                self->redo.clear();
            }
            if (self->undoDirection == 0)
                self->redo.clear();
            if (self->undoDirection != 0 && !self->pendingUndo.empty()) {
                std::vector<files::Change> remaining;
                for (auto &original : self->pendingUndo) {
                    bool done = false;
                    for (auto &completed : progress.changes)
                        done |= !_wcsicmp(original.result.c_str(), completed.source.c_str());
                    if (!done)
                        remaining.push_back(original);
                }
                if (!remaining.empty())
                    (self->undoDirection == 1 ? self->undo : self->redo).push_back(std::move(remaining));
                self->pendingUndo.clear();
            }
            self->undoDirection = 0;
            if (!self->pendingOpen.empty()) {
                if (SUCCEEDED(progress.error) && !progress.cancelled &&
                    GetFileAttributesW(self->pendingOpen.c_str()) != INVALID_FILE_ATTRIBUTES)
                    openTarget(self->pendingOpen);
                self->pendingOpen.clear();
            }
            self->selection.clear();
            self->selected = -1;
            self->preserve.clear();
            for (auto &change : progress.changes)
                if (fs::path(change.result).parent_path() == fs::path(self->path))
                    self->preserve.insert(change.result);
            self->requestListing();
            if (progress.cancelled)
                self->status = L"Операция отменена. Завершено: " + std::to_wstring(progress.changes.size());
            else if (FAILED(progress.error))
                self->status = progress.name.starts_with(L"ZIP: ") ? progress.name : files::errorText(progress.error);
            else if (progress.skipped)
                self->status = L"Пропущено: " + std::to_wstring(progress.skipped);
            else
                self->status.clear();
            SetPropW(w, L"OwnIsland.Files.OperationError", (HANDLE)(UINT_PTR)(DWORD)progress.error);
            self->draw();
        }
        return 0;
    }
    case WM_TIMER:
        if (a == 8) {
            KillTimer(w, 8);
            if (self->path == L"::recycle")
                self->refresh();
            return 0;
        }
        if (a == 5) {
            KillTimer(w, 5);
            self->resize();
        } else if (a == 6) {
            auto now = GetTickCount64();
            if (self->changeEvent != INVALID_HANDLE_VALUE &&
                WaitForSingleObject(self->changeEvent, 0) == WAIT_OBJECT_0) {
                FindNextChangeNotification(self->changeEvent);
                self->lastChange = now;
                self->refreshPending = true;
            }
            if (self->refreshPending && !self->busy() && !self->loading && now - self->lastChange >= 400) {
                self->refreshPending = false;
                self->refresh();
            }
            if (self->busy() && !IsIconic(w))
                self->draw();
        }
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(b), y = GET_Y_LPARAM(b);
        if (self->sizingColumn >= 0) {
            self->columnWidth[self->sizingColumn] =
                std::clamp(self->columnOriginal + x / self->scale - self->columnStart,
                           self->sizingColumn == 0 ? 120.f : 64.f, 900.f);
            self->draw();
            return 0;
        }
        if (self->marquee) {
            float px = x / self->scale, py = y / self->scale, sx = self->dragStart.x / self->scale,
                  sy = self->dragStart.y / self->scale;
            self->marqueeBox = {std::min(px, sx), std::min(py, sy), std::abs(px - sx), std::abs(py - sy)};
            self->selection = self->marqueeInitial;
            for (int pos = self->scroll; pos < (int)self->visible.size() && pos < self->scroll + self->capacity();
                 pos++) {
                auto r = self->itemBox(pos), q = self->marqueeBox;
                if (r.x < q.x + q.w && r.x + r.w > q.x && r.y < q.y + q.h && r.y + r.h > q.y)
                    self->selection.insert(self->visible[pos]);
            }
            self->draw();
            return 0;
        }
        if (self->scrollDrag) {
            int total = ((int)self->visible.size() + self->columns() - 1) / self->columns();
            float fraction = std::clamp((y / self->scale - 181) / (self->height - 226), 0.f, 1.f);
            self->scroll = (int)(std::max(0, total - self->rows()) * fraction) * self->columns();
            self->draw();
            return 0;
        }
        if (self->dragPending && (a & MK_LBUTTON) &&
            (abs(x - self->dragStart.x) > GetSystemMetrics(SM_CXDRAG) ||
             abs(y - self->dragStart.y) > GetSystemMetrics(SM_CYDRAG))) {
            self->dragPending = false;
            ReleaseCapture();
            self->drag();
            return 0;
        }
        int n = self->rowAt(x, y);
        if (n != self->hover) {
            self->hover = n;
            self->draw();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(w);
        float x = GET_X_LPARAM(b) / self->scale, y = GET_Y_LPARAM(b) / self->scale;
        int edge = self->columnEdge(x, y);
        if (edge >= 0) {
            self->sizingColumn = edge;
            self->columnStart = x;
            self->columnOriginal = self->columnBoxes()[edge].w;
            SetCapture(w);
            return 0;
        }
        if (x > self->right() - 16 && x < self->right() && y >= 180 && y < self->height - 42) {
            self->scrollDrag = true;
            SetCapture(w);
            return 0;
        }
        for (auto it = self->hits.rbegin(); it != self->hits.rend(); ++it)
            if (it->rect.contains(x, y)) {
                self->action(it->action);
                return 0;
            }
        int i = self->rowAt(GET_X_LPARAM(b), GET_Y_LPARAM(b));
        bool ctrl = (a & MK_CONTROL) != 0, shift = (a & MK_SHIFT) != 0;
        if (i < 0 || !self->selection.count(i) || ctrl || shift)
            self->choose(i, ctrl, shift);
        else
            self->selected = i;
        if (i < 0 && x >= self->left() && x < self->right() && y >= 180 && y < self->height - 42) {
            self->marquee = true;
            self->marqueeInitial = self->selection;
            self->dragStart = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
            self->marqueeBox = {x, y, 0, 0};
            SetCapture(w);
        }
        if (i >= 0) {
            self->dragPending = true;
            self->dragStart = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
            SetCapture(w);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (self->sizingColumn >= 0) {
            self->sizingColumn = -1;
            self->savePreferences();
        }
        self->marquee = false;
        self->dragPending = false;
        self->scrollDrag = false;
        if (GetCapture() == w)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        self->marquee = self->dragPending = self->scrollDrag = false;
        return 0;
    case WM_LBUTTONDBLCLK:
        self->dragPending = false;
        if (GetCapture() == w)
            ReleaseCapture();
        self->selected = self->rowAt(GET_X_LPARAM(b), GET_Y_LPARAM(b));
        self->openSelected();
        return 0;
    case WM_MBUTTONUP: {
        float x = GET_X_LPARAM(b) / self->scale, y = GET_Y_LPARAM(b) / self->scale;
        for (auto h : self->hits)
            if (h.action >= 1000 && h.action < 1012 && h.rect.contains(x, y)) {
                self->closeTab(h.action - 1000);
                break;
            }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (p.x == -1 && p.y == -1) {
            int pos = self->position(self->selected);
            auto r = pos < 0 ? Box{self->left(), 180, 20, 20} : self->itemBox(pos);
            p = {(LONG)((r.x + 20) * self->scale), (LONG)((r.y + 20) * self->scale)};
            ClientToScreen(w, &p);
        } else {
            POINT local = p;
            ScreenToClient(w, &local);
            if (local.x / self->scale >= self->left() && local.y / self->scale >= 146 && local.y / self->scale < 178) {
                self->columnsMenu(p);
                return 0;
            }
            if (local.x / self->scale < self->left() && local.y / self->scale >= 146) {
                int i = self->sideScroll + (int)((local.y / self->scale - 146) / 32);
                if (i >= 0 && i < (int)self->sidebar.size() && self->sideFavorite[i]) {
                    int r = customMenu(w, p, {{L"Открыть в новой вкладке", nullptr, 1}, {L"Открепить", nullptr, 2}},
                                       palette);
                    if (r == 1)
                        self->newTab(self->sidebar[i].second);
                    if (r == 2) {
                        std::erase(self->favorites, self->sidebar[i].second);
                        self->saveFavorites();
                        self->draw();
                    }
                    return 0;
                }
            }
            int i = self->rowAt(local.x, local.y);
            if (i < 0 || !self->selection.count(i))
                self->choose(i);
        }
        self->draw();
        self->context(p);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        ScreenToClient(w, &p);
        if (self->left() > 20 && p.x / self->scale < self->left() && p.y / self->scale >= 146) {
            self->sideScroll -= GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3;
            self->draw();
            return 0;
        }
    }
        if (GET_KEYSTATE_WPARAM(a) & MK_CONTROL) {
            self->grid = GET_WHEEL_DELTA_WPARAM(a) > 0;
            self->savePreferences();
            self->scroll = 0;
            self->refresh();
        } else {
            self->scroll -= GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3 * self->columns();
            self->draw();
        }
        return 0;
    case WM_XBUTTONUP:
        self->action(GET_XBUTTON_WPARAM(a) == XBUTTON1 ? 1 : 10);
        return TRUE;
    case WM_COMMAND:
        if (!b && HIWORD(a) == 0) {
            self->action(LOWORD(a));
            return 0;
        }
        break;
    case WM_KEYDOWN:
        self->key(a);
        return 0;
    case WM_SYSKEYDOWN:
        if (a == 'D') {
            self->beginAddress();
            SendMessageW(self->pathEdit, EM_SETSEL, 0, -1);
            return 0;
        }
        if (a == 'P') {
            self->previewEnabled = !self->previewEnabled;
            self->requestPreview();
            self->draw();
            return 0;
        }
        if (a == VK_LEFT)
            self->action(1);
        else if (a == VK_RIGHT)
            self->action(10);
        else if (a == VK_UP)
            self->action(2);
        else if (a == VK_RETURN)
            files::properties(w, self->selectedPaths());
        else
            break;
        return 0;
    case WM_CHAR:
        if (a >= 32 && a != 127) {
            static std::wstring find;
            static ULONGLONG previous = 0;
            auto now = GetTickCount64();
            if (now - previous > 900)
                find.clear();
            previous = now;
            find += (wchar_t)a;
            auto needle = lower(find);
            for (auto i : self->visible)
                if (lower(self->listing->entries[i].name).rfind(needle, 0) == 0) {
                    self->choose(i);
                    self->ensureVisible();
                    self->draw();
                    break;
                }
        }
        return 0;
    case WM_SETCURSOR: {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(w, &p);
        if (self->sizingColumn >= 0 || self->columnEdge(p.x / self->scale, p.y / self->scale) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
    }
        if (LOWORD(b) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)a, RGB((palette.text >> 16) & 255, (palette.text >> 8) & 255, palette.text & 255));
        SetBkColor((HDC)a, RGB((palette.card >> 16) & 255, (palette.card >> 8) & 255, palette.card & 255));
        return (LRESULT)self->editBrush;
    case WM_CLOSE:
        self->action(103);
        return 0;
    case WM_DESTROY: {
        KillTimer(w, 6);
        RevokeDragDrop(w);
        if (self->drop) {
            self->drop->Release();
            self->drop = nullptr;
        }
        std::erase(windows, w);
        HWND island = FindWindowW(L"OwnIsland.Surface", nullptr);
        if (GetPropW(island, L"OwnIsland.FilesWindow") == w) {
            RemovePropW(island, L"OwnIsland.FilesWindow");
            if (!windows.empty())
                SetPropW(island, L"OwnIsland.FilesWindow", windows.back());
        }
        return 0;
    }
    case WM_NCDESTROY: {
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        bool own = self->owned;
        auto result = DefWindowProcW(w, m, a, b);
        if (own)
            delete self;
        return result;
    }
    }
    return DefWindowProcW(w, m, a, b);
}
} // namespace
bool openFileBrowser(const std::wstring &requested, bool selectFile, bool newWindow) {
    if(CurrentComponent!=ComponentRole::Files)return routeFileBrowser(requested,selectFile,newWindow);
    std::wstring path = requested.empty() ? home() : requested, selected;
    std::error_code ec;
    if (path.rfind(L"::", 0) != 0 && !shellPath(path) && (selectFile || !fs::is_directory(path, ec))) {
        auto p = fs::path(path);
        selected = p.filename().wstring();
        path = p.parent_path().wstring();
    }
    if (!newWindow && !windows.empty() && IsWindow(windows.back())) {
        HWND w = windows.back();
        auto browser = (Browser *)GetWindowLongPtrW(w, GWLP_USERDATA);
        if (!requested.empty()) {
            browser->selectName = selected;
            browser->load(path);
        }
        if (IsIconic(w))
            ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        return true;
    }
    WNDCLASSEXW c{sizeof(c)};
    c.lpfnWndProc = Browser::proc;
    c.hInstance = GetModuleHandleW(nullptr);
    c.lpszClassName = Class;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.style = CS_DBLCLKS;
    c.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&c);
    auto storage = std::make_unique<Browser>();
    auto browser = storage.get();
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    int x = monitor.rcWork.left + std::max<LONG>(0, (monitor.rcWork.right - monitor.rcWork.left - 1120) / 2),
        y = monitor.rcWork.top + 80;
    HWND w = CreateWindowExW(WS_EX_APPWINDOW, Class, L"Файлы",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, x, y, 1120,
                             720, nullptr, nullptr, c.hInstance, browser);
    if (!w)
        return false;
    browser->owned = true;
    storage.release();
    windows.push_back(w);
    browser->selectName = selected;
    browser->load(path);
    SetPropW(FindWindowW(L"OwnIsland.Surface", nullptr), L"OwnIsland.FilesWindow", w);
    ShowWindow(w, SW_SHOWNORMAL);
    InvalidateRect(w, nullptr, FALSE);
    SetForegroundWindow(w);
    return true;
}
bool openFileLocation(const std::wstring &path) { return openFileBrowser(path, true); }
void fileBrowserColors(MenuColors colors) {
    palette = colors;
    for (HWND w : windows)
        if (IsWindow(w)) {
            auto b = (Browser *)GetWindowLongPtrW(w, GWLP_USERDATA);
            if (b->editBrush)
                DeleteObject(b->editBrush);
            b->editBrush =
                CreateSolidBrush(RGB((palette.card >> 16) & 255, (palette.card >> 8) & 255, palette.card & 255));
            InvalidateRect(w, nullptr, FALSE);
            InvalidateRect(b->pathEdit, nullptr, TRUE);
            InvalidateRect(b->searchEdit, nullptr, TRUE);
        }
}
bool fileOperationsActive() {
    for (HWND w : windows)
        if (IsWindow(w)) {
            auto b = (Browser *)GetWindowLongPtrW(w, GWLP_USERDATA);
            if (b && b->busy())
                return true;
        }
    return false;
}
size_t fileBrowserWindowCount(){return windows.size();}
HWND lastFileBrowserWindow(){return windows.empty()?nullptr:windows.back();}
void closeFileBrowsers() {
    auto copy = windows;
    for (HWND w : copy)
        if (IsWindow(w))
            SendMessageW(w, WM_CLOSE, 0, 0);
}
void fileBrowserDisplayChanged() {
    if(CurrentComponent!=ComponentRole::Files){componentFilesDisplayChanged();return;}
    for (HWND w : windows) {
        if (!IsWindow(w) || IsIconic(w)) continue;
        RECT r{};
        if (!GetWindowRect(w, &r)) continue;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &monitor)) continue;
        RECT overlap{};
        if (IntersectRect(&overlap, &r, &monitor.rcWork) && overlap.right-overlap.left >= 120 && overlap.bottom-overlap.top >= 64) continue;
        int width = std::min(r.right-r.left, monitor.rcWork.right-monitor.rcWork.left);
        int height = std::min(r.bottom-r.top, monitor.rcWork.bottom-monitor.rcWork.top);
        SetWindowPos(w, nullptr, monitor.rcWork.left + (monitor.rcWork.right-monitor.rcWork.left-width)/2,
                     monitor.rcWork.top + (monitor.rcWork.bottom-monitor.rcWork.top-height)/2, width, height,
                     SWP_NOZORDER|SWP_NOACTIVATE);
    }
}
