// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "tray.h"
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <shellapi.h>
#include <windowsx.h>

namespace {
constexpr DWORD Signature = 0x34753423;
constexpr wchar_t EndpointMarker[] = L"OwnIsland.TrayEndpoint";
struct Icon {
  HWND window = nullptr;
  DWORD pid = 0;
  UINT id = 0, callback = 0, version = 0, flags = 0, state = 0;
  GUID guid{};
  bool byGuid = false;
  std::wstring tip;
  std::shared_ptr<const PixelImage> image;
};
struct Wire {
  UINT operation = 0;
  HWND window = nullptr;
  UINT id = 0, flags = 0, callback = 0, state = 0, mask = 0, version = 0,
       infoFlags = 0;
  HICON icon = nullptr;
  GUID guid{};
  std::wstring tip, info, title;
};
DWORD word(const unsigned char *data, size_t size, size_t offset) {
  DWORD n = 0;
  if (offset <= size && size - offset >= 4)
    memcpy(&n, data + offset, 4);
  return n;
}
std::wstring stringAt(const unsigned char *data, size_t size, size_t offset,
                      size_t capacity) {
  if (offset >= size)
    return {};
  size_t count = std::min(capacity, (size - offset) / 2);
  std::wstring out(count, 0);
  memcpy(out.data(), data + offset, count * 2);
  out.resize(wcsnlen(out.data(), count));
  return out;
}
bool decode(const COPYDATASTRUCT &data, Wire &out) {
  if (data.dwData != 1 || !data.lpData || data.cbData < 32 ||
      data.cbData > 65536)
    return false;
  auto p = (const unsigned char *)data.lpData;
  size_t n = data.cbData;
  if (word(p, n, 0) != Signature)
    return false;
  out.operation = word(p, n, 4);
  if (out.operation > NIM_SETVERSION)
    return false;
  DWORD structure = word(p, n, 8);
  if (structure < 152 || structure > 4096)
    return false;
  UINT flags = word(p, n, 20);
  if (((flags & NIF_TIP) && n < 160) || ((flags & NIF_STATE) && n < 296) ||
      ((flags & NIF_INFO) && n < 944) || ((flags & NIF_GUID) && n < 960) ||
      (out.operation == NIM_SETVERSION && n < 812))
    return false;
  out.window = (HWND)(UINT_PTR)word(p, n, 12);
  out.id = word(p, n, 16);
  out.flags = word(p, n, 20);
  out.callback = word(p, n, 24);
  out.icon = (HICON)(UINT_PTR)word(p, n, 28);
  out.tip = stringAt(p, n, 32, n < 288 ? 64 : 128);
  out.state = word(p, n, 288);
  out.mask = word(p, n, 292);
  out.info = stringAt(p, n, 296, 256);
  out.version = word(p, n, 808);
  out.title = stringAt(p, n, 812, 64);
  out.infoFlags = word(p, n, 940);
  if (n >= 960)
    memcpy(&out.guid, p + 944, sizeof(GUID));
  if ((out.flags & NIF_GUID) && n < 960)
    return false;
  return out.operation == NIM_ADD ? IsWindow(out.window) != FALSE
                                  : out.window || (out.flags & NIF_GUID);
}
bool same(const Icon &icon, HWND hwnd, UINT id, const GUID &guid, bool byGuid) {
  return byGuid ? icon.byGuid && IsEqualGUID(icon.guid, guid)
                : !icon.byGuid && icon.window == hwnd && icon.id == id;
}
bool live(const Icon &icon) {
  DWORD pid = 0;
  return IsWindow(icon.window) && GetWindowThreadProcessId(icon.window, &pid) &&
         pid == icon.pid;
}
void callback(const Icon &icon, UINT event, POINT at) {
  if (!live(icon) || !icon.callback)
    return;
  AllowSetForegroundWindow(icon.pid);
  WPARAM wp = icon.version >= 4 ? MAKEWPARAM((WORD)at.x, (WORD)at.y) : icon.id;
  LPARAM lp = icon.version >= 4 ? MAKELPARAM(event, icon.id) : event;
  PostMessageW(icon.window, icon.callback, wp, lp);
}
} // namespace
bool isIslandTrayEndpoint(HWND window) {
  return GetPropW(window, EndpointMarker) != nullptr;
}
struct TrayHost::Impl {
  HWND owner = nullptr, endpoint = nullptr, panel = nullptr, notice = nullptr;
  MenuColors look{0x09090b, 0x252328, 0xeae9ed, 0x8a8790, 0xb8d8c9};
  std::unique_ptr<Graphics> gfx, noticeGfx;
  std::vector<Icon> icons;
  uint64_t revision = 1ull << 60;
  float scale = 1;
  bool awake = true;
  POINT anchor{};
  int hover = -1, rowOffset = 0;
  ULONGLONG contextUntil = 0;
  DWORD contextPid = 0;
  struct Balloon {
    Icon icon;
    std::wstring title, text;
  };
  std::deque<Balloon> queue;
  Balloon balloon;
  ULONGLONG noticeUntil = 0;
  static LRESULT CALLBACK endpointProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK panelProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK noticeProc(HWND, UINT, WPARAM, LPARAM);
  std::vector<size_t> visibleItems() const {
    std::vector<size_t> out;
    for (size_t i = 0; i < icons.size(); i++)
      if (!(icons[i].state & NIS_HIDDEN))
        out.push_back(i);
    return out;
  }
  int at(int x, int y) {
    auto shown = visibleItems();
    int col = (int)(x / scale - 8) / 44, row = (int)(y / scale - 8) / 44;
    if (x / scale < 8 || y / scale < 8 || col < 0 || col >= 5 || row < 0 ||
        row >= 8)
      return -1;
    size_t index = (row + rowOffset) * 5 + col;
    return index < shown.size() ? (int)shown[index] : -1;
  }
  RECT iconRect(size_t index) {
    auto shown = visibleItems();
    auto at = std::find(shown.begin(), shown.end(), index);
    size_t n = at == shown.end() ? 0 : at - shown.begin();
    RECT r{};
    GetWindowRect(panel, &r);
    int x = r.left + (int)((8 + (n % 5) * 44) * scale),
        y = r.top + (int)((8 + ((int)n / 5 - rowOffset) * 44) * scale);
    return {x, y, x + (LONG)(44 * scale), y + (LONG)(44 * scale)};
  }
  void place() {
    auto shown = visibleItems();
    int rows = std::clamp((int)((shown.size() + 4) / 5), 1, 8);
    rowOffset = std::clamp(rowOffset, 0,
                           std::max(0, (int)((shown.size() + 4) / 5) - 8));
    int w = (int)(236 * scale), h = (int)((rows * 44 + 42) * scale);
    MONITORINFO m{sizeof(m)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &m);
    int x = std::clamp(anchor.x - w, m.rcWork.left,
                       std::max(m.rcWork.left, m.rcWork.right - w)),
        y = std::clamp(anchor.y, m.rcWork.top,
                       std::max(m.rcWork.top, m.rcWork.bottom - h));
    SetWindowPos(panel, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (gfx)
      gfx->resize(w, h, scale);
  }
  void draw() {
    if (!gfx)
      return;
    gfx->begin();
    RECT r{};
    GetClientRect(panel, &r);
    gfx->fill({0, 0, r.right / scale, r.bottom / scale}, look.background, 12);
    auto shown = visibleItems();
    for (size_t n = 0; n < shown.size(); n++) {
      int row = (int)n / 5 - rowOffset;
      if (row < 0 || row >= 8)
        continue;
      auto i = shown[n];
      Box b{8 + (n % 5) * 44.f, 8 + row * 44.f, 44, 44};
      if ((int)i == hover)
        gfx->fill({b.x + 2, b.y + 2, 40, 40}, look.card, 9);
      if (icons[i].image)
        gfx->pixels(icons[i].image, {b.x + 9, b.y + 9, 26, 26});
      else
        gfx->icon(L"\uE8A5", b, 20, look.muted);
    }
    std::wstring text = hover >= 0 && hover < (int)icons.size()
                            ? icons[hover].tip
                        : shown.empty() ? L"Нет значков"
                                        : L"";
    gfx->text(text, {14, r.bottom / scale - 32, 208, 26}, 10, look.muted, false,
              DWRITE_TEXT_ALIGNMENT_CENTER);
    gfx->end();
  }
  void announce() {
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0,
                 0);
  }
  void notifyChanged() {
    SetPropW(endpoint, L"OwnIsland.Tray.Count", (HANDLE)(UINT_PTR)icons.size());
    if (IsWindowVisible(panel)) {
      place();
      draw();
    }
    PostMessageW(owner, WM_APP + 8, 0, 0);
  }
  LRESULT packet(const COPYDATASTRUCT &data) {
    if (data.dwData == 3 && data.lpData && data.cbData >= 40 &&
        data.cbData <= 128) {
      auto p = (const unsigned char *)data.lpData;
      if (word(p, data.cbData, 0) != Signature)
        return 0;
      GUID guid{};
      memcpy(&guid, p + 24, 16);
      bool byGuid = !IsEqualGUID(guid, GUID{});
      HWND hwnd = (HWND)(UINT_PTR)word(p, data.cbData, 16);
      UINT id = word(p, data.cbData, 20);
      for (size_t i = 0; i < icons.size(); i++)
        if (same(icons[i], hwnd, id, guid, byGuid)) {
          auto r = iconRect(i);
          return word(p, data.cbData, 4) == 2
                     ? MAKELRESULT(r.right - r.left, r.bottom - r.top)
                     : MAKELRESULT(r.left, r.top);
        }
      return 0;
    }
    Wire w;
    if (!decode(data, w))
      return 0;
    bool guid = (w.flags & NIF_GUID) != 0;
    auto at = std::find_if(icons.begin(), icons.end(), [&](auto &icon) {
      return same(icon, w.window, w.id, w.guid, guid);
    });
    if (w.operation == NIM_DELETE) {
      if (at == icons.end())
        return 0;
      removeNotices(*at);
      icons.erase(at);
      hover = -1;
      notifyChanged();
      return 1;
    }
    if (at != icons.end() && !live(*at)) {
      removeNotices(*at);
      icons.erase(at);
      hover = -1;
      notifyChanged();
      if (w.operation != NIM_ADD) return 0;
      at = icons.end();
    }
    if (w.operation == NIM_SETFOCUS) {
      contextUntil = 0;
      contextPid = 0;
      if (at != icons.end() && IsWindowVisible(panel))
        SetForegroundWindow(panel);
      return at != icons.end();
    }
    if (w.operation == NIM_SETVERSION) {
      if (at == icons.end() || w.version > 4)
        return 0;
      at->version = w.version;
      return 1;
    }
    if (at == icons.end()) {
      if (w.operation != NIM_ADD || icons.size() >= 512)
        return 0;
      Icon icon;
      icon.window = w.window;
      GetWindowThreadProcessId(w.window, &icon.pid);
      icon.id = w.id;
      icon.guid = w.guid;
      icon.byGuid = guid;
      icons.push_back(std::move(icon));
      at = icons.end() - 1;
    }
    if (w.operation == NIM_ADD) {
      at->version = 0;
      at->window = w.window;
      GetWindowThreadProcessId(w.window, &at->pid);
    }
    if (w.flags & NIF_MESSAGE)
      at->callback = w.callback;
    if (w.flags & NIF_TIP)
      at->tip = w.tip;
    if (w.flags & NIF_ICON) {
      HICON copy = w.icon ? CopyIcon(w.icon) : nullptr;
      at->image = copyIconPixels(copy, revision++);
      if (copy)
        DestroyIcon(copy);
    }
    if (w.flags & NIF_STATE)
      at->state = (at->state & ~w.mask) | (w.state & w.mask);
    at->flags = w.flags;
    if ((w.flags & NIF_INFO) && w.info.empty()) {
      auto matches = [&](const Icon &i) {
        return same(i, at->window, at->id, at->guid, at->byGuid);
      };
      std::erase_if(queue, [&](auto &b) { return matches(b.icon); });
      if (IsWindowVisible(notice) && matches(balloon.icon))
        finishNotice(NIN_BALLOONHIDE);
    }
    if ((w.flags & NIF_INFO) && !w.info.empty()) {
      QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
      SHQueryUserNotificationState(&state);
      if (!((w.infoFlags & NIIF_RESPECT_QUIET_TIME) &&
            state != QUNS_ACCEPTS_NOTIFICATIONS) &&
          !((w.flags & NIF_REALTIME) && (!awake || IsWindowVisible(notice)))) {
        std::erase_if(queue, [&](auto &b) {
          return same(b.icon, at->window, at->id, at->guid, at->byGuid);
        });
        if (queue.size() < 8)
          queue.push_back({*at, w.title, w.info});
        showNext();
      }
    }
    notifyChanged();
    return 1;
  }
  void removeNotices(const Icon &icon) {
    auto match = [&](const Icon &other) {
      return same(other, icon.window, icon.id, icon.guid, icon.byGuid);
    };
    std::erase_if(queue, [&](auto &b) { return match(b.icon); });
    if (IsWindowVisible(notice) && match(balloon.icon)) {
      ShowWindow(notice, SW_HIDE);
      noticeUntil = 0;
    }
  }
  void setAwake(bool value) {
    if (awake == value)
      return;
    awake = value;
    if (!awake) {
      ShowWindow(panel, SW_HIDE);
      contextUntil = 0;
      finishNotice(NIN_BALLOONHIDE);
    } else
      showNext();
  }
  void showNext() {
    if (!awake || IsWindowVisible(notice) || queue.empty())
      return;
    while (!queue.empty() && !live(queue.front().icon))
      queue.pop_front();
    if (queue.empty())
      return;
    balloon = std::move(queue.front());
    queue.pop_front();
    MONITORINFO m{sizeof(m)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &m);
    int w = 360, h = 152;
    SetWindowPos(notice, HWND_TOPMOST, m.rcWork.right - w - 18,
                 m.rcWork.top + 58, w, h, SWP_NOACTIVATE);
    if (!noticeGfx) {
      noticeGfx = std::make_unique<Graphics>();
      if (!noticeGfx->init(notice, w, h, 1)) {
        noticeGfx.reset();
        callback(balloon.icon, NIN_BALLOONHIDE, {});
        return;
      }
    } else
      noticeGfx->resize(w, h, 1);
    drawNotice();
    ShowWindow(notice, SW_SHOWNOACTIVATE);
    noticeUntil = GetTickCount64() + 6000;
    callback(balloon.icon, NIN_BALLOONSHOW, {});
  }
  void drawNotice() {
    if (!noticeGfx)
      return;
    noticeGfx->begin();
    noticeGfx->fill({0, 0, 360, 152}, look.background, 12);
    if (balloon.icon.image)
      noticeGfx->pixels(balloon.icon.image, {17, 18, 28, 28});
    noticeGfx->text(balloon.title, {55, 13, 260, 34}, 13, look.text, true);
    noticeGfx->icon(L"\uE711", {326, 10, 24, 28}, 12, look.muted);
    noticeGfx->text(balloon.text, {20, 52, 320, 84}, 12, look.text, false,
                    DWRITE_TEXT_ALIGNMENT_LEADING, true);
    noticeGfx->end();
  }
  void finishNotice(UINT event) {
    if (!IsWindowVisible(notice))
      return;
    ShowWindow(notice, SW_HIDE);
    callback(balloon.icon, event, {});
    noticeUntil = 0;
    showNext();
  }
  void activate(int index, UINT msg, POINT position) {
    if (index < 0 || index >= (int)icons.size())
      return;
    auto icon = icons[index];
    if (msg == WM_RBUTTONUP) {
      contextUntil = GetTickCount64() + 5000;
      contextPid = icon.pid;
      callback(icon, icon.version >= NOTIFYICON_VERSION ? WM_CONTEXTMENU : msg,
               position);
    } else if (msg == WM_LBUTTONUP && icon.version >= NOTIFYICON_VERSION)
      callback(icon, NIN_SELECT, position);
    else if (!(msg == WM_RBUTTONDOWN && icon.version >= NOTIFYICON_VERSION))
      callback(icon, msg, position);
  }
};
LRESULT CALLBACK TrayHost::Impl::endpointProc(HWND hwnd, UINT msg, WPARAM wp,
                                              LPARAM lp) {
  auto self = (Impl *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    self = (Impl *)((CREATESTRUCTW *)lp)->lpCreateParams;
    self->endpoint = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  }
  if (!self)
    return DefWindowProcW(hwnd, msg, wp, lp);
  if (msg == WM_COPYDATA && lp)
    return self->packet(*(COPYDATASTRUCT *)lp);
  if (msg == WM_CLOSE)
    return 0;
  return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK TrayHost::Impl::panelProc(HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp) {
  auto self = (Impl *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    self = (Impl *)((CREATESTRUCTW *)lp)->lpCreateParams;
    self->panel = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  }
  if (!self)
    return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    self->draw();
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_SETCURSOR:
    SetCursor(LoadCursorW(nullptr, self->hover >= 0 ? IDC_HAND : IDC_ARROW));
    return TRUE;
  case WM_MOUSEMOVE: {
    int next = self->at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    if (next >= 0 && self->icons[next].version < 4) {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ClientToScreen(hwnd, &p);
      callback(self->icons[next], WM_MOUSEMOVE, p);
    }
    if (next != self->hover) {
      if (self->hover >= 0 && self->hover < (int)self->icons.size() &&
          self->icons[self->hover].version >= 4)
        callback(self->icons[self->hover], NIN_POPUPCLOSE, {});
      self->hover = next;
      if (next >= 0 && self->icons[next].version >= 4) {
        POINT p;
        GetCursorPos(&p);
        callback(self->icons[next], NIN_POPUPOPEN, p);
      }
      self->draw();
    }
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&track);
    return 0;
  }
  case WM_MOUSELEAVE:
    if (self->hover >= 0 && self->hover < (int)self->icons.size() &&
        self->icons[self->hover].version >= 4)
      callback(self->icons[self->hover], NIN_POPUPCLOSE, {});
    self->hover = -1;
    self->draw();
    return 0;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_LBUTTONDBLCLK:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP: {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    int index = self->at(p.x, p.y);
    ClientToScreen(hwnd, &p);
    self->activate(index, msg, p);
    return 0;
  }
  case WM_MOUSEWHEEL:
    self->rowOffset -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
    self->place();
    self->draw();
    return 0;
  case WM_KEYDOWN: {
    if (wp == VK_ESCAPE) {
      ShowWindow(hwnd, SW_HIDE);
      PostMessageW(self->owner, WM_APP + 8, 0, 0);
      return 0;
    }
    auto shown = self->visibleItems();
    if (shown.empty())
      return 0;
    auto selected =
        std::find(shown.begin(), shown.end(), (size_t)std::max(0, self->hover));
    int n = self->hover < 0 || selected == shown.end()
                ? -1
                : (int)(selected - shown.begin());
    if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) {
      int delta = wp == VK_LEFT    ? -1
                  : wp == VK_RIGHT ? 1
                  : wp == VK_UP    ? -5
                                   : 5;
      n = std::clamp(n < 0 ? 0 : n + delta, 0, (int)shown.size() - 1);
      self->hover = (int)shown[n];
      int row = n / 5;
      if (row < self->rowOffset)
        self->rowOffset = row;
      if (row >= self->rowOffset + 8)
        self->rowOffset = row - 7;
      self->place();
      self->draw();
    } else if (n >= 0 && (wp == VK_RETURN || wp == VK_SPACE || wp == VK_APPS ||
                          (wp == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)))) {
      auto r = self->iconRect(shown[n]);
      POINT p{r.left, r.top};
      bool context = wp == VK_APPS || wp == VK_F10;
      if (context)
        self->activate((int)shown[n], WM_RBUTTONUP, p);
      else
        callback(self->icons[shown[n]],
                 self->icons[shown[n]].version >= NOTIFYICON_VERSION
                     ? NIN_KEYSELECT
                     : WM_LBUTTONUP,
                 p);
    }
    return 0;
  }
  case WM_ACTIVATE:
    if (LOWORD(wp) == WA_INACTIVE && GetTickCount64() > self->contextUntil) {
      ShowWindow(hwnd, SW_HIDE);
      PostMessageW(self->owner, WM_APP + 8, 0, 0);
    }
    return 0;
  case WM_CLOSE:
    ShowWindow(hwnd, SW_HIDE);
    PostMessageW(self->owner, WM_APP + 8, 0, 0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK TrayHost::Impl::noticeProc(HWND hwnd, UINT msg, WPARAM wp,
                                            LPARAM lp) {
  auto self = (Impl *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    self = (Impl *)((CREATESTRUCTW *)lp)->lpCreateParams;
    self->notice = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  }
  if (!self)
    return DefWindowProcW(hwnd, msg, wp, lp);
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    self->drawNotice();
    return 0;
  }
  if (msg == WM_ERASEBKGND)
    return 1;
  if (msg == WM_MOUSEACTIVATE)
    return MA_NOACTIVATE;
  if (msg == WM_RBUTTONUP) {
    self->finishNotice(NIN_BALLOONHIDE);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    self->finishNotice(GET_X_LPARAM(lp) > 320 ? NIN_BALLOONHIDE
                                              : NIN_BALLOONUSERCLICK);
    return 0;
  }
  if (msg == WM_CLOSE) {
    self->finishNotice(NIN_BALLOONHIDE);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
TrayHost::TrayHost() = default;
TrayHost::~TrayHost() { stop(); }
bool TrayHost::start(HWND owner) {
  if (impl)
    return true;
  impl = std::make_unique<Impl>();
  impl->owner = owner;
  WNDCLASSEXW c{sizeof(c)};
  c.hInstance = GetModuleHandleW(nullptr);
  c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  c.lpfnWndProc = Impl::endpointProc;
  c.lpszClassName = L"Shell_TrayWnd";
  RegisterClassExW(&c);
  impl->endpoint =
      CreateWindowExW(WS_EX_TOOLWINDOW, c.lpszClassName, L"", WS_POPUP, 0, 0, 0,
                      0, nullptr, nullptr, c.hInstance, impl.get());
  if (!impl->endpoint) {
    impl.reset();
    return false;
  }
  SetPropW(impl->endpoint, EndpointMarker, (HANDLE)1);
  c.lpfnWndProc = Impl::panelProc;
  c.lpszClassName = L"OwnIsland.Tray";
  c.style = CS_DBLCLKS;
  RegisterClassExW(&c);
  impl->panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                    WS_EX_NOREDIRECTIONBITMAP,
                                c.lpszClassName, L"", WS_POPUP, 0, 0, 236, 86,
                                nullptr, nullptr, c.hInstance, impl.get());
  c.lpfnWndProc = Impl::noticeProc;
  c.lpszClassName = L"OwnIsland.TrayNotice";
  c.style = 0;
  RegisterClassExW(&c);
  impl->notice = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                     WS_EX_NOREDIRECTIONBITMAP,
                                 c.lpszClassName, L"", WS_POPUP, 0, 0, 360, 152,
                                 nullptr, nullptr, c.hInstance, impl.get());
  if (!impl->panel || !impl->notice) {
    stop();
    return false;
  }
  impl->anchor = {300, 60};
  impl->place();
  impl->announce();
  return true;
}
void TrayHost::stop() {
  if (!impl)
    return;
  impl->gfx.reset();
  impl->noticeGfx.reset();
  if (IsWindow(impl->panel))
    DestroyWindow(impl->panel);
  if (IsWindow(impl->notice))
    DestroyWindow(impl->notice);
  if (IsWindow(impl->endpoint))
    DestroyWindow(impl->endpoint);
  impl.reset();
}
void TrayHost::setAwake(bool awake) {
  if (impl)
    impl->setAwake(awake);
}
bool TrayHost::hosted() const { return impl != nullptr; }
bool TrayHost::visible() const { return impl && IsWindowVisible(impl->panel); }
bool TrayHost::contextInteraction() const {
  return impl && GetTickCount64() < impl->contextUntil;
}
void TrayHost::close() {
  if (impl) {
    impl->contextUntil = 0;
    ShowWindow(impl->panel, SW_HIDE);
  }
}
void TrayHost::colors(MenuColors colors) {
  if (impl) {
    impl->look = colors;
    if (visible())
      impl->draw();
    if (IsWindowVisible(impl->notice))
      impl->drawNotice();
  }
}
bool TrayHost::toggle(POINT anchor) {
  if (!impl)
    return false;
  if (visible()) {
    close();
    return true;
  }
  impl->anchor = anchor;
  impl->scale = GetDpiForWindow(impl->owner) / 96.f;
  impl->place();
  if (!impl->gfx) {
    RECT r{};
    GetClientRect(impl->panel, &r);
    impl->gfx = std::make_unique<Graphics>();
    if (!impl->gfx->init(impl->panel, r.right, r.bottom, impl->scale)) {
      impl->gfx.reset();
      return false;
    }
  }
  impl->draw();
  ShowWindow(impl->panel, SW_SHOWNOACTIVATE);
  SetForegroundWindow(impl->panel);
  return true;
}
unsigned TrayHost::count() const {
  return impl ? (unsigned)impl->icons.size() : 0;
}
void TrayHost::maintain() {
  if (!impl)
    return;
  size_t n = impl->icons.size();
  std::erase_if(impl->icons, [&](auto &i) {
    if (live(i))
      return false;
    impl->removeNotices(i);
    return true;
  });
  impl->showNext();
  if (n != impl->icons.size()) {
    impl->hover = -1;
    impl->notifyChanged();
  }
  if (impl->noticeUntil && GetTickCount64() >= impl->noticeUntil)
    impl->finishNotice(NIN_BALLOONTIMEOUT);
  if (visible() && impl->contextUntil &&
      GetTickCount64() >= impl->contextUntil) {
    GUITHREADINFO info{sizeof(info)};
    GetGUIThreadInfo(0, &info);
    DWORD foregroundPid = 0;
    HWND foreground = GetForegroundWindow();
    GetWindowThreadProcessId(foreground, &foregroundPid);
    bool popup = foregroundPid == impl->contextPid &&
                 (GetWindowLongPtrW(foreground, GWL_STYLE) & WS_POPUP);
    if ((info.flags & GUI_INMENUMODE) || popup)
      impl->contextUntil = GetTickCount64() + 500;
    else {
      impl->contextUntil = 0;
      HWND fg = GetForegroundWindow();
      if (fg != impl->panel)
        close();
    }
  }
}


