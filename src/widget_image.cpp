// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "widget_image.h"
#include "platform.h"
#include <wincodec.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
namespace {
std::atomic<uint64_t> imageId=1ull<<61;
std::wstring readText(const std::wstring& path){std::ifstream f{std::filesystem::path(path),std::ios::binary};std::string s((std::istreambuf_iterator<char>(f)),{});if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n);return w;}
void writeText(const std::wstring& path,const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string b(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),b.data(),n,nullptr,nullptr);std::ofstream f{std::filesystem::path(path),std::ios::binary|std::ios::trunc};f.write(b.data(),b.size());}
unsigned metadata(IWICMetadataQueryReader* q,const wchar_t* name,unsigned fallback=0){if(!q)return fallback;PROPVARIANT v{};unsigned out=fallback;if(SUCCEEDED(q->GetMetadataByName(name,&v))){if(v.vt==VT_UI1)out=v.bVal;else if(v.vt==VT_UI2)out=v.uiVal;else if(v.vt==VT_UI4)out=v.ulVal;else if(v.vt==VT_BOOL)out=v.boolVal!=VARIANT_FALSE;}PropVariantClear(&v);return out;}
void composite(std::vector<unsigned char>& target,unsigned tw,unsigned th,const std::vector<unsigned char>& source,unsigned sw,unsigned sh,unsigned left,unsigned top){for(unsigned y=0;y<sh&&y+top<th;y++)for(unsigned x=0;x<sw&&x+left<tw;x++){auto src=&source[((size_t)y*sw+x)*4];auto dst=&target[((size_t)(y+top)*tw+x+left)*4];unsigned inverse=255-src[3];for(int c=0;c<4;c++)dst[c]=(unsigned char)std::min(255u,src[c]+(dst[c]*inverse+127)/255);}}
struct GifFrames {
    Com<IWICImagingFactory> factory;Com<IWICBitmapDecoder> decoder;
    unsigned width=0,height=0,count=0,index=0,disposal=0,left=0,top=0,fw=0,fh=0;int delay=100;
    std::vector<unsigned char> canvas,restore;unsigned char background[4]={};
    void erase(unsigned l,unsigned t,unsigned w,unsigned h){for(unsigned y=t;y<std::min(height,t+h);y++)for(unsigned x=l;x<std::min(width,l+w);x++)std::copy_n(background,4,&canvas[((size_t)y*width+x)*4]);}
    bool open(const std::wstring& path){if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.put())))||FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,decoder.put())))return false;GUID kind{};decoder->GetContainerFormat(&kind);if(kind!=GUID_ContainerFormatGif||FAILED(decoder->GetFrameCount(&count))||!count||count>4096)return false;Com<IWICMetadataQueryReader> q;decoder->GetMetadataQueryReader(q.put());width=metadata(q.p,L"/logscrdesc/Width");height=metadata(q.p,L"/logscrdesc/Height");if(!width||!height||width>4096||height>4096||(uint64_t)width*height>8388608)return false;canvas.assign((size_t)width*height*4,0);Com<IWICBitmapFrameDecode> frame;Com<IWICMetadataQueryReader> fq;decoder->GetFrame(0,frame.put());if(frame)frame->GetMetadataQueryReader(fq.put());if(!metadata(fq.p,L"/grctlext/TransparencyFlag")&&metadata(q.p,L"/logscrdesc/GlobalColorTableFlag")){Com<IWICPalette> palette;factory->CreatePalette(palette.put());if(palette&&SUCCEEDED(decoder->CopyPalette(palette.p))){WICColor colors[256]={};UINT actual=0;palette->GetColors(256,colors,&actual);unsigned index=metadata(q.p,L"/logscrdesc/BackgroundColorIndex");if(index<actual){auto c=colors[index];background[0]=(unsigned char)c;background[1]=(unsigned char)(c>>8);background[2]=(unsigned char)(c>>16);background[3]=255;}}}return true;}
    std::shared_ptr<const PixelImage> next(bool publish=true){
        if(!index){erase(0,0,width,height);disposal=0;restore.clear();}
        if(disposal==2){erase(left,top,fw,fh);}else if(disposal==3&&restore.size()==canvas.size())canvas=restore;
        Com<IWICBitmapFrameDecode> frame;Com<IWICFormatConverter> convert;Com<IWICMetadataQueryReader> q;if(FAILED(decoder->GetFrame(index,frame.put())))return {};frame->GetMetadataQueryReader(q.put());frame->GetSize(&fw,&fh);left=metadata(q.p,L"/imgdesc/Left");top=metadata(q.p,L"/imgdesc/Top");disposal=metadata(q.p,L"/grctlext/Disposal");delay=std::clamp((int)metadata(q.p,L"/grctlext/Delay",10)*10,40,60000);if(!fw||!fh||(uint64_t)fw*fh>8388608||left>=width||top>=height)return {};if(disposal==3)restore=canvas;
        if(FAILED(factory->CreateFormatConverter(convert.put()))||FAILED(convert->Initialize(frame.p,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};std::vector<unsigned char> pixels((size_t)fw*fh*4);if(FAILED(convert->CopyPixels(nullptr,fw*4,(UINT)pixels.size(),pixels.data())))return {};composite(canvas,width,height,pixels,fw,fh,left,top);
        if(!publish){index=(index+1)%count;static auto skipped=std::make_shared<PixelImage>();return skipped;}auto image=std::make_shared<PixelImage>();float factor=std::min(1.f,1280.f/std::max(width,height));image->width=std::max(1u,(unsigned)std::lround(width*factor));image->height=std::max(1u,(unsigned)std::lround(height*factor));image->id=imageId++;image->bgra.resize((size_t)image->width*image->height*4);
        if(factor==1)image->bgra=canvas;else{Com<IWICBitmap> bitmap;Com<IWICBitmapScaler> scaled;if(FAILED(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppPBGRA,width*4,(UINT)canvas.size(),canvas.data(),bitmap.put()))||FAILED(factory->CreateBitmapScaler(scaled.put()))||FAILED(scaled->Initialize(bitmap.p,image->width,image->height,WICBitmapInterpolationModeFant))||FAILED(scaled->CopyPixels(nullptr,image->width*4,(UINT)image->bgra.size(),image->bgra.data())))return {};}
        index=(index+1)%count;return image;
    }
};
}
void WidgetImageMedia::start(const std::wstring& id){if(worker.joinable())return;key=id;stopping=false;worker=std::thread([this]{run();});}
void WidgetImageMedia::stop(){{std::lock_guard lock(mutex);stopping=true;}wake.notify_all();if(worker.joinable())worker.join();}
void WidgetImageMedia::choose(HWND owner){wchar_t path[32768]={};OPENFILENAMEW f{sizeof(f)};f.hwndOwner=owner;f.lpstrFile=path;f.nMaxFile=32768;f.lpstrTitle=L"Изображение или GIF";f.lpstrFilter=L"Изображения\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.tif;*.tiff\0Все файлы\0*.*\0";f.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(!GetOpenFileNameW(&f))return;load(path);}
void WidgetImageMedia::load(const std::wstring& path){{std::lock_guard lock(mutex);request=path;pending=true;++generation;state.loading=!path.empty();state.error.clear();}wake.notify_all();}
void WidgetImageMedia::clear(){{std::lock_guard lock(mutex);request.clear();pending=true;++generation;state.loading=false;}wake.notify_all();}
void WidgetImageMedia::setActive(bool value,int maximumFPS){bool changed;{std::lock_guard lock(mutex);maximumFPS=std::clamp(maximumFPS,5,60);changed=active!=value||fps!=maximumFPS;active=value;fps=maximumFPS;}if(changed)wake.notify_all();}
void WidgetImageMedia::run(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);auto root=stateDirectory(),metadataPath=root+L"\\"+key+L"-image.txt";auto saved=readText(metadataPath);size_t split=saved.find(L'\n');std::wstring oldCache=split==std::wstring::npos?L"":saved.substr(split+1);if(!oldCache.empty()){std::lock_guard lock(mutex);if(!pending){request=oldCache;pending=true;}}
    std::unique_ptr<GifFrames> gif;ULONGLONG due=0,lastPublish=0;uint64_t loaded=0;
    for(;;){std::wstring path;bool load=false;uint64_t revision;int maximum=30;
        {std::unique_lock lock(mutex);if(stopping)break;if(pending){path=request;pending=false;load=true;}else if(!active||!gif||gif->count<2){due=0;wake.wait(lock,[&]{return stopping||pending||(active&&gif&&gif->count>1);});continue;}else{auto now=GetTickCount64();if(!due)due=now+gif->delay;if(now<due){wake.wait_for(lock,std::chrono::milliseconds(due-now),[&]{return stopping||pending||!active;});continue;}}revision=generation;maximum=fps;}
        if(load){
            if(path.empty()){gif.reset();writeText(metadataPath,L"");if(!oldCache.empty()&&oldCache.rfind(root+L"\\"+key+L"-image-cache-",0)==0)DeleteFileW(oldCache.c_str());oldCache.clear();{std::lock_guard lock(mutex);if(revision==generation){state={};loaded=revision;}}continue;}
            bool animated=false;std::shared_ptr<const PixelImage> image;std::unique_ptr<GifFrames> candidate;auto ext=std::filesystem::path(path).extension().wstring();for(auto& c:ext)c=towlower(c);if(ext==L".gif"){candidate=std::make_unique<GifFrames>();if(candidate->open(path)){image=candidate->next();animated=candidate->count>1;}}else image=readWallpaper(path,imageId++);
            bool fresh=path!=oldCache;std::wstring cache=oldCache;if(image&&fresh){cache=root+L"\\"+key+L"-image-cache-"+std::to_wstring(imageId++)+ext;auto temporary=cache+L".tmp";if(!CopyFileW(path.c_str(),temporary.c_str(),FALSE)||!MoveFileExW(temporary.c_str(),cache.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());image.reset();}if(image&&candidate){candidate=std::make_unique<GifFrames>();if(!candidate->open(cache)||(image=candidate->next())==nullptr)image.reset();}}
            {std::lock_guard lock(mutex);if(revision!=generation){candidate.reset();if(fresh&&cache!=oldCache&&!cache.empty())DeleteFileW(cache.c_str());continue;}state.loading=false;if(!image){candidate.reset();if(fresh&&cache!=oldCache&&!cache.empty()&&cache.rfind(root+L"\\"+key+L"-image-cache-",0)==0)DeleteFileW(cache.c_str());state.error=L"Не удалось загрузить изображение";continue;}gif=std::move(candidate);state.image=image;state.animated=animated;state.error.clear();state.name=std::filesystem::path(path).filename().wstring();loaded=revision;if(fresh){writeText(metadataPath,path+L"\n"+cache);if(!oldCache.empty()&&oldCache!=cache&&oldCache.rfind(root+L"\\"+key+L"-image-cache-",0)==0)DeleteFileW(oldCache.c_str());oldCache=cache;}due=0;}continue;
        }
        bool publish=GetTickCount64()-lastPublish>=(ULONGLONG)(1000/maximum);auto image=gif->next(publish);{std::lock_guard lock(mutex);if(stopping)break;if(!image){state.error=L"Не удалось прочитать кадр GIF";gif.reset();}else if(publish&&active&&generation==loaded){state.image=std::move(image);lastPublish=GetTickCount64();}due=GetTickCount64()+(gif?gif->delay:100);}
    }gif.reset();CoUninitialize();
}


