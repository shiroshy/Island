// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "runtime.h"
#include "file_browser.h"
#include "search.h"
#include "graphics.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#define DBINITCONSTANTS
#include <oledb.h>
#include <msdasc.h>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <set>
#include <cstddef>

namespace {
std::wstring fold(const std::wstring& text){
    if(text.empty())return {};
    std::wstring out(text.size(),L'\0');int n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,text.data(),(int)text.size(),out.data(),(int)out.size(),nullptr,nullptr,0);return n?out:text;
}
std::wstring trim(const std::wstring& text){auto a=text.find_first_not_of(L" \t\r\n"),b=text.find_last_not_of(L" \t\r\n");return a==std::wstring::npos?L"":text.substr(a,b-a+1);}
std::wstring expand(const std::wstring& text){DWORD n=ExpandEnvironmentStringsW(text.c_str(),nullptr,0);if(!n||n>32768)return text;std::wstring value(n,L'\0');ExpandEnvironmentStringsW(text.c_str(),value.data(),n);value.resize(n-1);return value;}
std::wstring extension(const std::wstring& path){return fold(std::filesystem::path(path).extension().wstring());}
bool executableType(const std::wstring& path){auto e=extension(path);return e==L".exe"||e==L".com"||e==L".bat"||e==L".cmd"||e==L".msc";}
std::wstring known(REFKNOWNFOLDERID id){PWSTR value=nullptr;std::wstring path;if(SUCCEEDED(SHGetKnownFolderPath(id,0,nullptr,&value))){path=value;CoTaskMemFree(value);}return path;}
std::wstring displayName(const std::wstring& path){SHFILEINFOW info{};if(SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_DISPLAYNAME)&&*info.szDisplayName){std::wstring name=info.szDisplayName;if(extension(name)==L".lnk"||extension(name)==L".exe")name=std::filesystem::path(name).stem().wstring();return name;}return std::filesystem::path(path).stem().wstring();}
SearchResult fromPath(const std::wstring& path,const std::wstring& name=L""){
    SearchResult item;item.path=path;item.target=path;item.name=name.empty()?displayName(path):name;
    DWORD attributes=GetFileAttributesW(path.c_str());item.directory=attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY);
    if(extension(path)==L".lnk"){
        Com<IShellLinkW> link;Com<IPersistFile> file;
        if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put())))&&SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.put())))&&SUCCEEDED(file->Load(path.c_str(),STGM_READ))){
            wchar_t target[32768]={},args[32768]={},directory[32768]={};
            link->GetPath(target,32768,nullptr,SLGP_RAWPATH);link->GetArguments(args,32768);link->GetWorkingDirectory(directory,32768);
            if(*target)item.target=expand(target);item.arguments=args;item.workingDirectory=expand(directory);
        }
        item.application=true;
    }else item.application=executableType(path)||extension(path)==L".appref-ms";
    item.administrator=!item.directory&&executableType(item.target);return item;
}
std::wstring resultKey(const SearchResult& item){return fold(item.application?item.target+L"|"+item.arguments:item.path);}
void addUnique(std::vector<SearchResult>& items,SearchResult item,std::set<std::wstring>& keys){if(item.path.empty()||item.name.empty())return;if(keys.insert(resultKey(item)).second)items.push_back(std::move(item));}
std::vector<SearchResult> programs(){
    std::vector<SearchResult> items;std::set<std::wstring> keys;
    for(auto root:{known(FOLDERID_Programs),known(FOLDERID_CommonPrograms)}){
        if(root.empty())continue;std::error_code error;
        for(std::filesystem::recursive_directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,error),end;it!=end;it.increment(error)){
            if(error){error.clear();continue;}if(it.depth()>10){it.disable_recursion_pending();continue;}
            auto ext=extension(it->path().wstring());if(ext==L".lnk"||ext==L".appref-ms"||ext==L".exe"||ext==L".msc")addUnique(items,fromPath(it->path().wstring()),keys);
        }
    }
    for(HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}){
        HKEY apps=nullptr;if(RegOpenKeyExW(hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths",0,KEY_READ,&apps)!=ERROR_SUCCESS)continue;
        for(DWORD index=0;;index++){wchar_t sub[512]={};DWORD length=512;if(RegEnumKeyExW(apps,index,sub,&length,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS)break;HKEY entry=nullptr;if(RegOpenKeyExW(apps,sub,0,KEY_QUERY_VALUE,&entry)!=ERROR_SUCCESS)continue;
            wchar_t path[32768]={};DWORD size=sizeof(path),type=0;if(RegQueryValueExW(entry,nullptr,nullptr,&type,(BYTE*)path,&size)==ERROR_SUCCESS&&(type==REG_SZ||type==REG_EXPAND_SZ)){path[32767]=0;auto value=trim(expand(path));if(value.size()>1&&value.front()==L'"'&&value.back()==L'"')value=value.substr(1,value.size()-2);if(GetFileAttributesW(value.c_str())!=INVALID_FILE_ATTRIBUTES)addUnique(items,fromPath(value),keys);}RegCloseKey(entry);
        }RegCloseKey(apps);
    }
    wchar_t system[32768]={},windows[32768]={};GetSystemDirectoryW(system,32768);GetWindowsDirectoryW(windows,32768);
    for(auto root:{std::wstring(system),std::wstring(windows)}){
        std::error_code error;for(std::filesystem::directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,error),end;it!=end;it.increment(error)){
            if(error){error.clear();continue;}auto path=it->path().wstring();if(executableType(path))addUnique(items,fromPath(path,it->path().stem().wstring()),keys);
        }
    }
    // AppsFolder supplies the same packaged-application names used by Windows.
    Com<IShellFolder> folder;Com<IEnumIDList> entries;PIDLIST_ABSOLUTE appRoot=nullptr;
    if(SUCCEEDED(SHParseDisplayName(L"shell:AppsFolder",nullptr,&appRoot,0,nullptr))){Com<IShellFolder> desktop;if(SUCCEEDED(SHGetDesktopFolder(desktop.put())))desktop->BindToObject(appRoot,nullptr,IID_PPV_ARGS(folder.put()));CoTaskMemFree(appRoot);}
    if(folder&&SUCCEEDED(folder->EnumObjects(nullptr,SHCONTF_NONFOLDERS,entries.put()))&&entries){
        PITEMID_CHILD child=nullptr;while(entries->Next(1,&child,nullptr)==S_OK){wchar_t name[1024]={},id[32768]={};STRRET text{};
            if(SUCCEEDED(folder->GetDisplayNameOf(child,SHGDN_NORMAL,&text)))StrRetToBufW(&text,child,name,1024);
            if(SUCCEEDED(folder->GetDisplayNameOf(child,SHGDN_FORPARSING,&text)))StrRetToBufW(&text,child,id,32768);
            std::wstring app=id;if(!app.empty()&&app.find(L'!')!=std::wstring::npos){SearchResult item;item.name=name;item.path=item.target=L"shell:AppsFolder\\"+app;item.application=true;addUnique(items,std::move(item),keys);}CoTaskMemFree(child);
        }
    }
    return items;
}
int match(const SearchResult& item,const std::wstring& query){
    auto name=fold(item.name),alias=fold(std::filesystem::path(item.target).stem().wstring()),filename=fold(std::filesystem::path(item.target).filename().wstring());
    if(name==query)return 0;if(alias==query||filename==query)return 3;if(name.starts_with(query))return 10;if(alias.starts_with(query)||filename.starts_with(query))return 15;if(name.find(query)!=std::wstring::npos)return 30;if(alias.find(query)!=std::wstring::npos||filename.find(query)!=std::wstring::npos)return 40;return 1000;
}
std::wstring containsWords(const std::wstring& text){
    std::vector<std::wstring> words;std::wstring word;
    for(wchar_t c:text){if(IsCharAlphaNumericW(c))word+=c;else if(!word.empty()){words.push_back(word);word.clear();}}if(!word.empty())words.push_back(word);
    std::wstring expression;for(auto& token:words){if(!expression.empty())expression+=L" AND ";expression+=L"\""+token+L"*\"";}
    return expression;
}
struct OleSession {
    Com<IDBInitialize> source;
    ~OleSession(){if(source)source->Uninitialize();}
};
long indexFiles(const std::wstring& query,std::vector<SearchResult>& items,const std::wstring& folder=L""){
    auto terms=containsWords(query);if(terms.empty())return 0;
    Com<IDataInitialize> initialize;
    HRESULT hr=CoCreateInstance(CLSID_MSDAINITIALIZE,nullptr,CLSCTX_INPROC_SERVER,IID_IDataInitialize,(void**)initialize.put());if(FAILED(hr))return hr;
    OleSession database;
    hr=initialize->GetDataSource(nullptr,CLSCTX_INPROC_SERVER,L"Provider=Search.CollatorDSO;Extended Properties=\"Application=Windows\"",IID_IDBInitialize,(IUnknown**)database.source.put());if(FAILED(hr))return hr;
    if(FAILED(hr=database.source->Initialize()))return hr;
    Com<IDBCreateSession> factory;Com<IDBCreateCommand> session;Com<ICommandText> command;
    if(FAILED(hr=database.source->QueryInterface(IID_IDBCreateSession,(void**)factory.put()))||FAILED(hr=factory->CreateSession(nullptr,IID_IDBCreateCommand,(IUnknown**)session.put()))||FAILED(hr=session->CreateCommand(nullptr,IID_ICommandText,(IUnknown**)command.put())))return hr;
    Com<ICommandProperties> properties;
    if(SUCCEEDED(command->QueryInterface(IID_ICommandProperties,(void**)properties.put()))){DBPROP timeout{};timeout.dwPropertyID=DBPROP_COMMANDTIMEOUT;timeout.dwOptions=DBPROPOPTIONS_OPTIONAL;timeout.colid=DB_NULLID;timeout.vValue.vt=VT_I4;timeout.vValue.lVal=4;DBPROPSET set{&timeout,1,DBPROPSET_ROWSET};properties->SetProperties(1,&set);}
    int limit=folder.empty()?64:2048;std::wstring scope=L"file:";if(!folder.empty()){wchar_t url[32768]={};DWORD size=32768;if(FAILED(UrlCreateFromPathW(folder.c_str(),url,&size,0)))return E_INVALIDARG;scope=url;}std::wstring escaped;for(auto c:scope){escaped+=c;if(c==L'\'')escaped+=c;}
    auto sql=L"SELECT TOP "+std::to_wstring(limit)+L" System.ItemNameDisplay, System.ItemUrl FROM SystemIndex WHERE SCOPE='"+escaped+L"' AND CONTAINS(System.FileName, '"+terms+L"') ORDER BY System.Search.Rank DESC";
    if(FAILED(hr=command->SetCommandText(DBGUID_DEFAULT,sql.c_str())))return hr;
    Com<IRowset> rows;if(FAILED(hr=command->Execute(nullptr,IID_IRowset,nullptr,nullptr,(IUnknown**)rows.put())))return hr;
    Com<IAccessor> accessor;if(FAILED(hr=rows->QueryInterface(IID_IAccessor,(void**)accessor.put())))return hr;
    struct CellName{DBSTATUS status;DBLENGTH length;wchar_t value[2048];};
    struct CellPath{DBSTATUS status;DBLENGTH length;wchar_t value[32768];};
    struct Row{CellName name;CellPath path;};
    DBBINDING bindings[2]{};size_t offsets[]={offsetof(Row,name),offsetof(Row,path)},sizes[]={sizeof(CellName::value),sizeof(CellPath::value)};
    for(int i=0;i<2;i++){auto& b=bindings[i];b.iOrdinal=i+1;b.obStatus=offsets[i];b.obLength=offsets[i]+offsetof(CellPath,length);b.obValue=offsets[i]+offsetof(CellPath,value);b.dwPart=DBPART_VALUE|DBPART_STATUS|DBPART_LENGTH;b.dwMemOwner=DBMEMOWNER_CLIENTOWNED;b.eParamIO=DBPARAMIO_NOTPARAM;b.cbMaxLen=sizes[i];b.wType=DBTYPE_WSTR;}
    HACCESSOR handle=0;DBBINDSTATUS status[2]{};if(FAILED(hr=accessor->CreateAccessor(DBACCESSOR_ROWDATA,2,bindings,sizeof(Row),&handle,status)))return hr;
    for(int count=0;count<limit;count++){
        DBCOUNTITEM obtained=0;HROW* values=nullptr;hr=rows->GetNextRows(0,0,1,&obtained,&values);
        if(FAILED(hr)||!obtained){if(values)CoTaskMemFree(values);break;}
        Row data{};auto got=rows->GetData(values[0],handle,&data);rows->ReleaseRows(obtained,values,nullptr,nullptr,nullptr);CoTaskMemFree(values);
        data.name.value[2047]=0;data.path.value[32767]=0;
        if(SUCCEEDED(got)&&data.path.status==DBSTATUS_S_OK&&*data.path.value){
            wchar_t decoded[32768]={};DWORD length=32768;
            if(SUCCEEDED(PathCreateFromUrlW(data.path.value,decoded,&length,0))){std::wstring path=decoded;if(!path.starts_with(L"\\\\")&&GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)items.push_back(fromPath(path,data.name.value));}
        }
    }
    accessor->ReleaseAccessor(handle,nullptr);return FAILED(hr)?hr:0;
}
void icons(std::vector<SearchResult>& items,std::map<std::wstring,std::shared_ptr<const PixelImage>>& cache,uint64_t& nextId){
    // The renderer shares its bitmap cache with task icons, so use a disjoint ID range.
    for(size_t i=0;i<items.size()&&i<24;i++){
        auto& item=items[i];auto& image=cache[item.path];if(!image)image=loadShellIcon(item.path,nextId++);item.icon=image;
    }
    if(cache.size()>192){std::set<std::wstring> keep;for(auto& item:items)keep.insert(item.path);for(auto it=cache.begin();it!=cache.end();)if(!keep.contains(it->first))it=cache.erase(it);else ++it;}
}
SearchState find(const std::wstring& text,const std::vector<SearchResult>& catalog,const std::vector<std::wstring>& sources){
    SearchState state;state.query=text;auto query=fold(trim(text));if(query.empty())return state;
    std::set<std::wstring> keys;
    auto direct=expand(trim(text));if(direct.size()>1&&direct.front()==L'"'&&direct.back()==L'"')direct=direct.substr(1,direct.size()-2);
    if(GetFileAttributesW(direct.c_str())!=INVALID_FILE_ATTRIBUTES)addUnique(state.results,fromPath(direct),keys);
    std::vector<SearchResult> apps;for(auto& item:catalog)if(match(item,query)<1000)apps.push_back(item);
    for(auto& path:sources){auto item=fromPath(path);if(match(item,query)<1000)apps.push_back(std::move(item));}
    std::stable_sort(apps.begin(),apps.end(),[&](auto& a,auto& b){int x=match(a,query),y=match(b,query);return x==y?fold(a.name)<fold(b.name):x<y;});
    for(auto& item:apps){addUnique(state.results,std::move(item),keys);if(state.results.size()>=48)break;}
    return state;
}
}
void WindowsSearch::start(HWND window){owner=window;stopping=false;worker=std::thread([this]{run();});}
void WindowsSearch::stop(){{std::lock_guard lock(mutex);stopping=true;}wake.notify_all();if(worker.joinable())worker.join();}
void WindowsSearch::request(const std::wstring& text){std::lock_guard lock(mutex);query=text.substr(0,512);++revision;dirty=true;auto next=std::make_shared<SearchState>();next->query=query;next->revision=revision;next->busy=!trim(query).empty();current=next;wake.notify_one();}
void WindowsSearch::setSources(const std::vector<std::wstring>& paths){std::lock_guard lock(mutex);sources=paths;}
std::shared_ptr<const SearchState> WindowsSearch::snapshot(){std::lock_guard lock(mutex);return current;}
void WindowsSearch::run(){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    std::vector<SearchResult> catalog;ULONGLONG catalogAt=0;std::map<std::wstring,std::shared_ptr<const PixelImage>> cache;uint64_t iconId=1ull<<48;
    while(true){
        std::wstring text;std::vector<std::wstring> extra;uint64_t version;
        {std::unique_lock lock(mutex);wake.wait(lock,[this]{return stopping||dirty;});if(stopping)break;version=revision;
            if(wake.wait_for(lock,std::chrono::milliseconds(180),[&]{return stopping||revision!=version;}))continue;
            text=query;extra=sources;dirty=false;}
        if(trim(text).empty())continue;
        if(!catalogAt||GetTickCount64()-catalogAt>60000){catalog=programs();catalogAt=GetTickCount64();}
        auto next=std::make_shared<SearchState>(find(text,catalog,extra));next->revision=version;next->busy=true;icons(next->results,cache,iconId);
        {std::lock_guard lock(mutex);if(stopping)break;if(version!=revision)continue;current=next;}PostMessageW(owner,WM_APP+12,0,0);
        std::vector<SearchResult> files;long error=indexFiles(text,files);
        {std::lock_guard lock(mutex);if(stopping)break;if(version!=revision)continue;}
        auto final=std::make_shared<SearchState>(*next);std::set<std::wstring> keys;for(auto& item:final->results)keys.insert(resultKey(item));for(auto& item:files)addUnique(final->results,std::move(item),keys);
        final->indexError=error;final->busy=false;icons(final->results,cache,iconId);
        {std::lock_guard lock(mutex);if(stopping)break;if(version!=revision)continue;current=final;}PostMessageW(owner,WM_APP+12,0,0);
    }
    CoUninitialize();
}
SearchState searchOnce(const std::wstring& text,const std::vector<std::wstring>& sources){auto state=find(text,programs(),sources);std::vector<SearchResult> files;state.indexError=indexFiles(text,files);std::set<std::wstring> keys;for(auto& item:state.results)keys.insert(resultKey(item));for(auto& item:files)addUnique(state.results,std::move(item),keys);return state;}
bool searchOpen(HWND owner,const SearchResult& item,bool administrator){
    if(administrator&&!item.administrator)return false;
    std::wstring path=administrator?item.target:item.path,args=administrator?item.arguments:L"";
    if(administrator&&extension(path)==L".msc"){args=L"\""+path+L"\"";path=L"mmc.exe";}
    SHELLEXECUTEINFOW execute{sizeof(execute)};execute.hwnd=owner;execute.fMask=SEE_MASK_FLAG_NO_UI;execute.lpVerb=administrator?L"runas":L"open";execute.lpFile=path.c_str();execute.lpParameters=args.empty()?nullptr:args.c_str();execute.lpDirectory=item.workingDirectory.empty()?nullptr:item.workingDirectory.c_str();execute.nShow=SW_SHOWNORMAL;{DWORD attrs=GetFileAttributesW(item.target.c_str());if(item.directory||(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY)))return openFileBrowser(item.target);if(!_wcsicmp(std::filesystem::path(item.target).filename().c_str(),L"explorer.exe"))return openFileBrowser();}return ShellExecuteExW(&execute)!=FALSE;
}
bool searchLocation(const SearchResult& item){if(StandaloneMode)return !item.path.starts_with(L"shell:")&&openFileLocation(item.path);if(item.path.starts_with(L"shell:"))return false;PIDLIST_ABSOLUTE id=nullptr;if(FAILED(SHParseDisplayName(item.path.c_str(),nullptr,&id,0,nullptr)))return false;HRESULT hr=SHOpenFolderAndSelectItems(id,0,nullptr,0);CoTaskMemFree(id);return SUCCEEDED(hr);}



SearchState searchFolder(const std::wstring& query,const std::wstring& folder){SearchState state;state.query=query;state.indexError=indexFiles(query,state.results,folder);return state;}
