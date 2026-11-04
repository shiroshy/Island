// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "wallpaper.h"
#include "graphics.h"
#include <wincodec.h>
#include <shlobj.h>
#include "platform.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <filesystem>
#include <fstream>
#include <atomic>

std::shared_ptr<const PixelImage> readWallpaper(const std::wstring& path,uint64_t id){
    WIN32_FILE_ATTRIBUTE_DATA attributes{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)||attributes.nFileSizeHigh||attributes.nFileSizeLow>100*1024*1024)return {};
    Com<IWICImagingFactory> factory;Com<IWICBitmapDecoder> decoder;Com<IWICBitmapFrameDecode> frame;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.put())))||FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,decoder.put()))||FAILED(decoder->GetFrame(0,frame.put())))return {};
    UINT width=0,height=0;if(FAILED(frame->GetSize(&width,&height))||!width||!height||(uint64_t)width*height>200000000)return {};
    IWICBitmapSource* source=frame.p;Com<IWICBitmapFlipRotator> orientation;Com<IWICMetadataQueryReader> metadata;
    if(SUCCEEDED(frame->GetMetadataQueryReader(metadata.put()))){PROPVARIANT value{};if(SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}",&value))&&value.vt==VT_UI2){WICBitmapTransformOptions transform=WICBitmapTransformRotate0;switch(value.uiVal){case 2:transform=WICBitmapTransformFlipHorizontal;break;case 3:transform=WICBitmapTransformRotate180;break;case 4:transform=WICBitmapTransformFlipVertical;break;case 5:transform=(WICBitmapTransformOptions)(WICBitmapTransformRotate90|WICBitmapTransformFlipHorizontal);break;case 6:transform=WICBitmapTransformRotate90;break;case 7:transform=(WICBitmapTransformOptions)(WICBitmapTransformRotate270|WICBitmapTransformFlipHorizontal);break;case 8:transform=WICBitmapTransformRotate270;break;}if(transform!=WICBitmapTransformRotate0&&SUCCEEDED(factory->CreateBitmapFlipRotator(orientation.put()))&&SUCCEEDED(orientation->Initialize(frame.p,transform))){source=orientation.p;source->GetSize(&width,&height);}}PropVariantClear(&value);}
    float fit=std::min(1.f,4096.f/std::max(width,height));UINT outWidth=std::max(1u,(UINT)(width*fit)),outHeight=std::max(1u,(UINT)(height*fit));Com<IWICBitmapScaler> scaler;
    if(fit<1){if(FAILED(factory->CreateBitmapScaler(scaler.put()))||FAILED(scaler->Initialize(source,outWidth,outHeight,WICBitmapInterpolationModeFant)))return {};source=scaler.p;}
    Com<IWICFormatConverter> converter;if(FAILED(factory->CreateFormatConverter(converter.put()))||FAILED(converter->Initialize(source,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
    auto image=std::make_shared<PixelImage>();image->width=outWidth;image->height=outHeight;image->id=id;image->bgra.resize((size_t)outWidth*outHeight*4);
    if(FAILED(converter->CopyPixels(nullptr,outWidth*4,(UINT)image->bgra.size(),image->bgra.data())))return {};return image;
}
std::wstring chooseWallpaper(HWND owner,bool video){
    Com<IFileOpenDialog> dialog;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.put()))))return {};
    COMDLG_FILTERSPEC filters[]={{video?L"Видео":L"Изображения",video?L"*.mp4;*.m4v;*.mov;*.wmv;*.avi":L"*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff"}};dialog->SetFileTypes(1,filters);dialog->SetTitle(video?L"Выбрать видеообои":L"Выбрать обои");dialog->SetOptions(FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_DONTADDTORECENT);
    if(FAILED(dialog->Show(owner)))return {};Com<IShellItem> item;if(FAILED(dialog->GetResult(item.put())))return {};PWSTR path=nullptr;std::wstring result;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){result=path;CoTaskMemFree(path);}return result;
}

namespace {
std::atomic<uint64_t> mediaImageId{(1ull<<61)};
std::wstring mediaText(const std::wstring& path){std::ifstream f(std::filesystem::path(path),std::ios::binary);std::string b((std::istreambuf_iterator<char>(f)),{});int n=MultiByteToWideChar(CP_UTF8,0,b.data(),(int)b.size(),nullptr,0);std::wstring s(n,0);if(n)MultiByteToWideChar(CP_UTF8,0,b.data(),(int)b.size(),s.data(),n);return s;}
bool mediaWrite(const std::wstring& path,const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string b(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),b.data(),n,nullptr,nullptr);std::ofstream f(std::filesystem::path(path),std::ios::binary);f.write(b.data(),b.size());return f.good();}
bool videoPath(const std::wstring& p){auto e=std::filesystem::path(p).extension().wstring();for(auto& c:e)c=towlower(c);return e==L".mp4"||e==L".m4v"||e==L".mov"||e==L".wmv"||e==L".avi";}
struct VideoFrames {
    Com<IMFSourceReader> reader;UINT32 width=0,height=0;LONG stride=0;LONGLONG timestamp=0;
    bool open(const std::wstring& path){
        reader.reset();Com<IMFAttributes> a;MFCreateAttributes(a.put(),2);a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
        if(FAILED(MFCreateSourceReaderFromURL(path.c_str(),a.p,reader.put())))return false;
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE);reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
        Com<IMFMediaType> type;MFCreateMediaType(type.put());type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
        if(FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.p)))return false;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,type.put());MFGetAttributeSize(type.p,MF_MT_FRAME_SIZE,&width,&height);
        if(!width||!height||width>4096||height>4096)return false;
        UINT32 s=0;stride=SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&s))?(LONG)s:(LONG)width*4;return true;
    }
    std::shared_ptr<const PixelImage> next(bool& ended){
        ended=false;Com<IMFSample> sample;DWORD flags=0;
        for(int attempts=0;attempts<12;attempts++){
            if(FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&timestamp,sample.put())))return {};
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ended=true;return {};}
            if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED){Com<IMFMediaType> type;if(FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,type.put()))||FAILED(MFGetAttributeSize(type.p,MF_MT_FRAME_SIZE,&width,&height))||!width||!height||width>4096||height>4096)return {};UINT32 s=0;stride=SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&s))?(LONG)s:(LONG)width*4;}
            if(sample)break;
        }
        if(!sample)return {};Com<IMFMediaBuffer> buffer;if(FAILED(sample->ConvertToContiguousBuffer(buffer.put())))return {};
        BYTE* bytes=nullptr;DWORD length=0;if(FAILED(buffer->Lock(&bytes,nullptr,&length)))return {};
        auto image=std::make_shared<PixelImage>();image->width=width;image->height=height;image->id=mediaImageId++;
        size_t pitch=std::abs(stride);bool valid=pitch>=width*4&&length>=pitch*(height-1)+width*4;
        if(valid){image->bgra.resize((size_t)width*height*4);for(UINT32 y=0;y<height;y++){auto row=bytes+(stride<0?height-1-y:y)*pitch;memcpy(image->bgra.data()+(size_t)y*width*4,row,width*4);}for(size_t i=3;i<image->bgra.size();i+=4)image->bgra[i]=255;}
        buffer->Unlock();return valid?image:nullptr;
    }
    bool rewind(){PROPVARIANT position{};position.vt=VT_I8;position.hVal.QuadPart=0;return SUCCEEDED(reader->SetCurrentPosition(GUID_NULL,position));}
};
}
void WallpaperMedia::start(const std::wstring& key,bool enabled){if(worker.joinable())return;prefix=key;state.enabled=enabled;stopping=false;worker=std::thread([this]{run();});}
void WallpaperMedia::stop(){{std::lock_guard l(mutex);stopping=true;}wake.notify_all();if(worker.joinable())worker.join();}
void WallpaperMedia::choose(HWND owner,bool video){auto path=chooseWallpaper(owner,video);if(path.empty())return;{std::lock_guard l(mutex);request=path;++generation;state.loading=true;state.error.clear();}wake.notify_all();}
void WallpaperMedia::clear(){{std::lock_guard l(mutex);++generation;request.clear();state={};}wake.notify_all();}
void WallpaperMedia::enable(bool value){{std::lock_guard l(mutex);state.enabled=value;}wake.notify_all();}
void WallpaperMedia::setActive(bool value){bool changed;{std::lock_guard l(mutex);changed=active!=value;active=value;}if(changed)wake.notify_all();}
WallpaperState WallpaperMedia::snapshot(){std::lock_guard l(mutex);return state;}
void WallpaperMedia::run(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool mf=SUCCEEDED(MFStartup(MF_VERSION));
    auto root=stateDirectory(),ini=root+L"\\settings.ini",metadata=root+L"\\"+prefix+L"-wallpaper-source.txt";
    auto saved=mediaText(metadata);auto split=saved.find(L'\n');std::wstring oldCache=split==std::wstring::npos?L"":saved.substr(split+1),original=split==std::wstring::npos?L"":saved.substr(0,split);
    if(oldCache.empty()&&prefix==L"desktop"){oldCache=root+L"\\wallpaper-cache";original=mediaText(root+L"\\wallpaper-path.txt");}
    bool initial=true;uint64_t loaded=~0ull;VideoFrames video;ULONGLONG base=0;LONGLONG first=0;bool timing=false;
    while(true){std::wstring path;uint64_t revision=0;bool use=false,enabled=false;
        {std::unique_lock l(mutex);if(stopping)break;revision=generation;enabled=state.enabled;use=active&&enabled;
            if(!request.empty()){path=request;request.clear();initial=false;}else if(initial){initial=false;if(enabled)path=oldCache;}
            if(path.empty()&&(!use||!state.video||!video.reader)){if(!use)timing=false;if(revision!=loaded&&!state.enabled)video.reader.reset();wake.wait_for(l,std::chrono::milliseconds(200));continue;}}
        if(!path.empty()){
            VideoFrames nextVideo;bool isVideo=videoPath(path),ended=false;auto image=isVideo?(mf&&nextVideo.open(path)?nextVideo.next(ended):nullptr):readWallpaper(path,mediaImageId++);
            std::wstring cache=path,temp;bool fresh=path!=oldCache;
            if(image&&fresh){auto ext=std::filesystem::path(path).extension().wstring();for(auto& c:ext)c=towlower(c);cache=root+L"\\"+prefix+L"-wallpaper-cache-"+std::to_wstring(image->id)+L"-"+std::to_wstring(GetTickCount64())+ext;temp=cache+L".tmp";if(!CopyFileW(path.c_str(),temp.c_str(),FALSE))image.reset();}
            {std::lock_guard l(mutex);if(stopping){if(!temp.empty())DeleteFileW(temp.c_str());break;}if(revision!=generation){if(!temp.empty())DeleteFileW(temp.c_str());continue;}state.loading=false;
                if(image&&fresh&&!MoveFileExW(temp.c_str(),cache.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))image.reset();
                if(!temp.empty())DeleteFileW(temp.c_str());
                if(image&&fresh&&isVideo){nextVideo.reader.reset();if(!nextVideo.open(cache))image.reset();else image=nextVideo.next(ended);}
                if(!image){if(fresh&&cache!=path&&cache.rfind(root+L"\\"+prefix+L"-wallpaper-cache",0)==0)DeleteFileW(cache.c_str());loaded=revision;state.error=isVideo?L"Видео не поддерживается или повреждено":L"Не удалось загрузить изображение";continue;}
                if(fresh){video.reader.reset();mediaWrite(metadata,path+L"\n"+cache);if(!oldCache.empty()&&oldCache!=cache&&oldCache.rfind(root+L"\\"+prefix+L"-wallpaper-cache",0)==0)DeleteFileW(oldCache.c_str());oldCache=cache;original=path;WritePrivateProfileStringW(prefix.c_str(),L"wallpaper-photo",L"1",ini.c_str());}
                state.image=image;state.name=std::filesystem::path(original.empty()?path:original).filename().wstring();state.error.clear();state.video=isVideo;state.enabled=true;loaded=revision;video=std::move(nextVideo);timing=false;
            }continue;
        }
        if(loaded!=revision){video.reader.reset();continue;}
        bool ended=false;auto image=video.next(ended);if(ended){if(!video.rewind())video.reader.reset();timing=false;continue;}
        if(!image){std::lock_guard l(mutex);state.error=L"Не удалось прочитать кадр видео";video.reader.reset();continue;}
        if(!timing){base=GetTickCount64();first=video.timestamp;timing=true;}
        auto due=base+(ULONGLONG)std::max<LONGLONG>(0,(video.timestamp-first)/10000);
        {std::unique_lock l(mutex);if(!active||!state.enabled){timing=false;continue;}auto now=GetTickCount64();if(due>now)wake.wait_for(l,std::chrono::milliseconds(std::min<ULONGLONG>(due-now,1000)),[&]{return stopping||generation!=loaded||!active;});if(stopping)break;if(generation==loaded&&active)state.image=std::move(image);}
    }
    video.reader.reset();if(mf)MFShutdown();CoUninitialize();
}
