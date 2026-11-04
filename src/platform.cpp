// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "health.h"
#include "file_browser.h"
#include "platform.h"
#include "runtime.h"
#include <shlobj.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <exception>
#include <dwmapi.h>
#include <winhttp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Data.Json.h>
#include <fstream>
#include <filesystem>
#include <cwctype>
#include <chrono>
#include <algorithm>

void MediaBridge::start(HWND target) {
    worker = std::thread([this,target] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            using namespace winrt::Windows::Media::Control;
            using namespace winrt::Windows::Storage::Streams;
            auto request = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            if (request.wait_for(std::chrono::seconds(4)) != winrt::Windows::Foundation::AsyncStatus::Completed) { request.Cancel(); return; }
            auto manager = request.GetResults();
            std::wstring previousKey;
            unsigned long long revision=0;
            MediaState current;
            while (!stopping) {
                try {
                    auto session = manager.GetCurrentSession();
                    if (session) {
                        auto action = command.exchange(0);
                        auto boundedWait = [](auto op) {
                            if (op.wait_for(std::chrono::seconds(2)) == winrt::Windows::Foundation::AsyncStatus::Completed) op.GetResults();
                            else op.Cancel();
                        };
                        if (action == 1) boundedWait(session.TryTogglePlayPauseAsync());
                        if (action == 2) boundedWait(session.TrySkipPreviousAsync());
                        if (action == 3) boundedWait(session.TrySkipNextAsync());
                        double seek = seekTo.exchange(-1);
                        if (seek >= 0) boundedWait(session.TryChangePlaybackPositionAsync(session.GetTimelineProperties().StartTime().count()+(int64_t)(seek*10000000)));
                        auto propertiesOp = session.TryGetMediaPropertiesAsync();
                        if (propertiesOp.wait_for(std::chrono::seconds(2)) != winrt::Windows::Foundation::AsyncStatus::Completed) { propertiesOp.Cancel(); continue; }
                        auto p = propertiesOp.GetResults();
                        current.available = true;
                        current.title = p.Title().c_str();
                        current.artist = p.Artist().c_str();
                        current.source = session.SourceAppUserModelId().c_str();
                        auto playback = session.GetPlaybackInfo();
                        current.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                        auto timeline = session.GetTimelineProperties();
                        current.position = (timeline.Position()-timeline.StartTime()).count()/10000000.0;
                        current.duration = (timeline.EndTime()-timeline.StartTime()).count()/10000000.0;
                        if(current.playing && timeline.LastUpdatedTime().time_since_epoch().count()>0)
                            current.position+=(winrt::clock::now()-timeline.LastUpdatedTime()).count()/10000000.0;
                        current.position=std::clamp(current.position,0.,std::max(0.,current.duration));
                        auto key = current.title + L"\n" + current.artist + L"\n" + current.source;
                        if (key != previousKey) {
                            previousKey = key;
                            current.cover.reset();
                            current.revision=++revision;
                            if (auto thumbnail = p.Thumbnail()) {
                                auto streamOp = thumbnail.OpenReadAsync();
                                if (streamOp.wait_for(std::chrono::seconds(2)) == winrt::Windows::Foundation::AsyncStatus::Completed) {
                                    auto stream = streamOp.GetResults();
                                    uint32_t size = (uint32_t)std::min<uint64_t>(stream.Size(), 4*1024*1024);
                                    if (size) {
                                        auto dataOp = stream.ReadAsync(Buffer(size), size, InputStreamOptions::None);
                                        if (dataOp.wait_for(std::chrono::seconds(2)) == winrt::Windows::Foundation::AsyncStatus::Completed) {
                                            auto data = dataOp.GetResults();
                                            auto cover=std::make_shared<std::vector<unsigned char>>(data.Length());
                                            DataReader::FromBuffer(data).ReadBytes(*cover);current.cover=std::move(cover);
                                        } else dataOp.Cancel();
                                    }
                                } else streamOp.Cancel();
                            }
                        }
                    } else {
                        if (current.available) { current = {}; current.revision=++revision; previousKey.clear(); }
                        command.store(0);
                    }
                    { std::lock_guard lock(mutex); state = current; }
                    PostMessageW(target, WM_APP+1, 0, 0);
                } catch (...) { /* A player can close between any two WinRT calls. */ }
                for (int n=0;n<10 && !stopping && !command.load() && seekTo.load()<0;n++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } catch (...) { /* Media remains an honest empty state when Windows has no SMTC. */ }
    });
}
void MediaBridge::stop() { stopping = true; if (worker.joinable()) worker.join(); }
MediaState MediaBridge::snapshot() { std::lock_guard lock(mutex); return state; }
std::wstring stateDirectory() {
    if(!StateDirectoryOverride.empty()){std::error_code ec;std::filesystem::create_directories(StateDirectoryOverride,ec);return StateDirectoryOverride;}
    wchar_t path[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path);
    std::wstring dir = std::wstring(path)+L"\\OwnIsland";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
std::wstring readNote() {
    std::ifstream file(std::filesystem::path(stateDirectory()+L"\\note.txt"),std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    int n = MultiByteToWideChar(CP_UTF8,0,bytes.data(),(int)bytes.size(),nullptr,0);
    std::wstring out(n,L'\0');
    if (n) MultiByteToWideChar(CP_UTF8,0,bytes.data(),(int)bytes.size(),out.data(),n);
    return out;
}
void writeNote(const std::wstring& value) {
    uint64_t noteHash=1469598103934665603ull;for(auto c:stateDirectory()){noteHash^=(uint16_t)c;noteHash*=1099511628211ull;}
    struct NoteLock{HANDLE h=nullptr;bool held=false;~NoteLock(){if(held)ReleaseMutex(h);if(h)CloseHandle(h);}} guard;
    guard.h=CreateMutexW(nullptr,FALSE,(L"Local\\OwnIsland.Note."+std::to_wstring(noteHash)).c_str());if(!guard.h)return;auto acquired=WaitForSingleObject(guard.h,2000);guard.held=acquired==WAIT_OBJECT_0||acquired==WAIT_ABANDONED;if(!guard.held)return;
    int n = WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);
    std::string bytes(n,'\0');
    if (n) WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),bytes.data(),n,nullptr,nullptr);
    auto path = stateDirectory()+L"\\note.txt";
    auto temporary=path+L".tmp."+std::to_wstring(GetCurrentProcessId());
    { std::ofstream file(std::filesystem::path(temporary),std::ios::binary); file.write(bytes.data(),bytes.size()); if (!file) return; }
    if(!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))DeleteFileW(temporary.c_str());
}
std::wstring clipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    std::wstring value;
    UINT privateFormat = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (!IsClipboardFormatAvailable(privateFormat) && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
                size_t max = std::min<size_t>(GlobalSize(h)/sizeof(wchar_t),32000);
                size_t len=0; while (len<max && p[len]) ++len;
                value.assign(p,len); GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    return value;
}
bool setClipboard(HWND owner, const std::wstring& value) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
    if (!mem) return false;
    void* p=GlobalLock(mem);
    if (!p) { GlobalFree(mem); return false; }
    memcpy(p,value.c_str(),(value.size()+1)*sizeof(wchar_t)); GlobalUnlock(mem);
    if (!OpenClipboard(owner)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT,mem)!=nullptr;
    CloseClipboard(); if (!ok) GlobalFree(mem);
    return ok;
}
std::wstring chooseFile(HWND owner, const wchar_t* title) {
    health::Hold protection;if(!protection)return L"";
    wchar_t path[32768] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner=owner; ofn.lpstrFile=path; ofn.nMaxFile=32768; ofn.lpstrTitle=title;
    ofn.Flags=OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? path : L"";
}
std::wstring fileName(const std::wstring& path) { auto p=path.find_last_of(L"\\/"); return p==std::wstring::npos ? path : path.substr(p+1); }
void openTarget(const std::wstring& target) { health::Hold protection;if(!protection)return; {DWORD a=GetFileAttributesW(target.c_str());auto filename=std::filesystem::path(target).filename().wstring();if(!_wcsicmp(std::filesystem::path(target).extension().c_str(),L".lnk")){openPinnedTarget(target,nullptr);return;}if((a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))||!_wcsicmp(filename.c_str(),L"explorer.exe")){openFileBrowser(!_wcsicmp(filename.c_str(),L"explorer.exe")?L"":target);return;}}ShellExecuteW(nullptr,L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
static Com<IAudioEndpointVolume> endpoint(ERole role, EDataFlow flow) {
    Com<IAudioEndpointVolume> volume;
    Com<IMMDeviceEnumerator> devices;
    Com<IMMDevice> device;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)devices.put())) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(flow,role,device.put())))
        device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,(void**)volume.put());
    return volume;
}
float getVolume() { auto p=endpoint(eMultimedia,eRender); float v=0; if(p) p->GetMasterVolumeLevelScalar(&v); return v; }
bool getMuted() { auto p=endpoint(eMultimedia,eRender); BOOL b=FALSE; if(p) p->GetMute(&b); return b; }
int getMicState() { auto p=endpoint(eCommunications,eCapture);if(!p)p=endpoint(eConsole,eCapture);BOOL mute=FALSE;return p&&SUCCEEDED(p->GetMute(&mute))?(mute?1:0):-1; }
void setVolume(float v) { auto p=endpoint(eMultimedia,eRender); if(p) p->SetMasterVolumeLevelScalar(std::clamp(v,0.f,1.f),nullptr); }
void toggleMute() { auto p=endpoint(eMultimedia,eRender); if(p) { BOOL b; if(SUCCEEDED(p->GetMute(&b))) p->SetMute(!b,nullptr); } }
void toggleMic() { auto p=endpoint(eCommunications,eCapture); if(p) { BOOL b; if(SUCCEEDED(p->GetMute(&b))) p->SetMute(!b,nullptr); } }
void captureScreen(HWND island) {
    ShowWindow(island,SW_HIDE);
    DwmFlush();
    int x=GetSystemMetrics(SM_XVIRTUALSCREEN), y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w=GetSystemMetrics(SM_CXVIRTUALSCREEN), h=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC screen=GetDC(nullptr), mem=CreateCompatibleDC(screen);
    HBITMAP bmp=CreateCompatibleBitmap(screen,w,h);
    auto old=SelectObject(mem,bmp);
    bool ok=BitBlt(mem,0,0,w,h,screen,x,y,SRCCOPY | CAPTUREBLT)!=FALSE;
    SelectObject(mem,old); DeleteDC(mem); ReleaseDC(nullptr,screen);
    if(ok && OpenClipboard(island)) { EmptyClipboard(); if(SetClipboardData(CF_BITMAP,bmp)) bmp=nullptr; CloseClipboard(); }
    if(bmp) DeleteObject(bmp);
    ShowWindow(island,SW_SHOWNOACTIVATE);
}
std::wstring transformText(const std::wstring& value, int action) {
    std::wstring result=value;
    if(action==0 && !result.empty())CharUpperBuffW(result.data(),(DWORD)result.size());
    if(action==1 && !result.empty())CharLowerBuffW(result.data(),(DWORD)result.size());
    if(action==2) {
        const std::wstring en=L"qwertyuiop[]asdfghjkl;'zxcvbnm,./`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~";
        const std::wstring ru=L"йцукенгшщзхъфывапролджэячсмитьбю.ёЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,Ё";
        size_t latin=0,cyrillic=0;
        for(auto c:value) { if((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z'))++latin; if(c>=0x400&&c<=0x4ff)++cyrillic; }
        auto& from=latin>=cyrillic?en:ru; auto& to=latin>=cyrillic?ru:en;
        for(auto& c:result) { auto p=from.find(c); if(p!=std::wstring::npos && p<to.size()) c=to[p]; }
    }
    if(action==3) { bool start=true; for(auto& c:result) { if(start)CharUpperBuffW(&c,1);else CharLowerBuffW(&c,1); start=iswspace(c)!=0; } }
    if(action==4) { result.clear(); bool space=false; for(auto c:value) { if(iswspace(c)) {space=!result.empty();} else { if(space)result+=L' '; result+=c; space=false; } } }
    return result;
}

static std::wstring publicWeatherRequest(const std::wstring& url) {
    struct Internet { HINTERNET h=nullptr; ~Internet(){if(h)WinHttpCloseHandle(h);} } session,connection,request;
    session.h=WinHttpOpen(L"OwnIsland/0.1",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session.h)throw std::runtime_error("http session");
    WinHttpSetTimeouts(session.h,3000,3000,5000,5000);
    URL_COMPONENTS parts={sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=(DWORD)-1;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&parts))throw std::runtime_error("url");
    std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
    if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    connection.h=WinHttpConnect(session.h,host.c_str(),parts.nPort,0);
    if(!connection.h)throw std::runtime_error("connection");
    request.h=WinHttpOpenRequest(connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    if(!request.h || !WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request.h,nullptr))throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    DWORD status=0,len=sizeof(status);WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&len,WINHTTP_NO_HEADER_INDEX);
    if(status<200||status>=300)throw std::runtime_error("http status");
    std::string bytes;char buffer[16384];DWORD read=0;
    do {if(!WinHttpReadData(request.h,buffer,sizeof(buffer),&read))throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));bytes.append(buffer,read);if(bytes.size()>2*1024*1024)throw std::runtime_error("response size");}while(read);
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),(int)bytes.size(),nullptr,0);
    if(!n)throw std::runtime_error("utf8");
    std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),(int)bytes.size(),out.data(),n);return out;
}
WeatherState fetchWeather(const std::wstring& city) {
    WeatherState out;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using namespace winrt::Windows::Foundation;
        using namespace winrt::Windows::Data::Json;
        auto fetch = [&](std::wstring url) {
            return JsonObject::Parse(publicWeatherRequest(url));
        };
        auto geo=fetch(L"https://geocoding-api.open-meteo.com/v1/search?name="+std::wstring(Uri::EscapeComponent(city))+L"&count=1&language=ru&format=json");
        if(!geo.HasKey(L"results") || geo.GetNamedArray(L"results").Size()==0) {out.error=L"Город не найден. Попробуйте другое название.";return out;}
        auto place=geo.GetNamedArray(L"results").GetObjectAt(0);
        out.city=place.GetNamedString(L"name").c_str();
        auto country=place.GetNamedString(L"country",L"");
        if(!country.empty())out.city+=L", "+std::wstring(country);
        double lat=place.GetNamedNumber(L"latitude"),lon=place.GetNamedNumber(L"longitude");
        auto data=fetch(L"https://api.open-meteo.com/v1/forecast?latitude="+std::to_wstring(lat)+L"&longitude="+std::to_wstring(lon)+L"&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=7");
        auto cur=data.GetNamedObject(L"current");
        out.temperature=cur.GetNamedNumber(L"temperature_2m");
        out.humidity=(int)cur.GetNamedNumber(L"relative_humidity_2m");
        out.wind=cur.GetNamedNumber(L"wind_speed_10m");
        out.code=(int)cur.GetNamedNumber(L"weather_code");
        auto daily=data.GetNamedObject(L"daily");
        auto dates=daily.GetNamedArray(L"time"),hi=daily.GetNamedArray(L"temperature_2m_max"),lo=daily.GetNamedArray(L"temperature_2m_min"),codes=daily.GetNamedArray(L"weather_code");
        for(uint32_t i=0;i<dates.Size();i++) out.days.push_back({dates.GetStringAt(i).c_str(),lo.GetNumberAt(i),hi.GetNumberAt(i),(int)codes.GetNumberAt(i)});
        out.available=true;
    }catch(winrt::hresult_error const& e){out.errorCode=(long)e.code().value;out.error=L"Не удалось получить прогноз. Проверьте подключение к интернету.";}
    catch(...){out.errorCode=-1;out.error=L"Не удалось получить прогноз. Проверьте подключение к интернету.";}
    return out;
}
