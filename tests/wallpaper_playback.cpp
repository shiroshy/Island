// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "../src/wallpaper.cpp"
#include <iostream>

std::wstring stateDirectory(){
    static std::wstring root=[] {wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);auto p=std::wstring(temp)+L"IslandWallpaperTest-"+std::to_wstring(GetCurrentProcessId());CreateDirectoryW(p.c_str(),nullptr);return p;}();return root;
}
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void fixture(const std::wstring& path){
    Com<IMFSinkWriter> writer;check(SUCCEEDED(MFCreateSinkWriterFromURL(path.c_str(),nullptr,nullptr,writer.put())),"sink");
    Com<IMFMediaType> output,input;MFCreateMediaType(output.put());output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264);output->SetUINT32(MF_MT_AVG_BITRATE,2000000);output->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);MFSetAttributeSize(output.p,MF_MT_FRAME_SIZE,640,360);MFSetAttributeRatio(output.p,MF_MT_FRAME_RATE,30,1);MFSetAttributeRatio(output.p,MF_MT_PIXEL_ASPECT_RATIO,1,1);
    DWORD stream;check(SUCCEEDED(writer->AddStream(output.p,&stream)),"stream");
    MFCreateMediaType(input.put());input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);input->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);MFSetAttributeSize(input.p,MF_MT_FRAME_SIZE,640,360);MFSetAttributeRatio(input.p,MF_MT_FRAME_RATE,30,1);MFSetAttributeRatio(input.p,MF_MT_PIXEL_ASPECT_RATIO,1,1);
    check(SUCCEEDED(writer->SetInputMediaType(stream,input.p,nullptr)),"input");check(SUCCEEDED(writer->BeginWriting()),"begin");
    for(int frame=0;frame<60;frame++){
        Com<IMFSample> sample;Com<IMFMediaBuffer> buffer;MFCreateSample(sample.put());MFCreateMemoryBuffer(640*360*4,buffer.put());BYTE* bytes;buffer->Lock(&bytes,nullptr,nullptr);for(int i=0;i<640*360;i++){bytes[i*4]=frame*4;bytes[i*4+1]=i/640<180?180:20;bytes[i*4+2]=100;bytes[i*4+3]=0;}buffer->Unlock();buffer->SetCurrentLength(640*360*4);sample->AddBuffer(buffer.p);sample->SetSampleTime(frame*10000000ll/30);sample->SetSampleDuration((frame+1)*10000000ll/30-frame*10000000ll/30);check(SUCCEEDED(writer->WriteSample(stream,sample.p)),"sample");
    }check(SUCCEEDED(writer->Finalize()),"finalize");
}
int wmain(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);int result=0;
    try{
        auto root=stateDirectory(),path=root+L"\\clip.mp4";fixture(path);
        {VideoFrames video;check(video.open(path),"open advanced");std::cout<<"D3D manager available: "<<(bool)video.manager<<"\n";bool ended=false;auto first=video.next(ended);check(first&&first->width==640&&first->height==360,"pixels");for(size_t i=3;i<first->bgra.size();i+=4)check(first->bgra[i]==255,"alpha");int count=1;while(video.read(ended))count++;check(ended&&count==60,"complete decode");check(video.rewind()&&video.next(ended)&&video.timestamp==0,"loop");check(video.open(path,true,false)&&video.next(ended),"CPU advanced fallback");check(video.open(path,false),"fallback open");auto legacy=video.next(ended);check(legacy!=nullptr,"fallback pixels");check(std::abs((int)first->bgra[1]-(int)legacy->bgra[1])<10&&std::abs((int)first->bgra[359*640*4+1]-(int)legacy->bgra[359*640*4+1])<10,"row orientation matches fallback");}
        mediaWrite(root+L"\\test-wallpaper-source.txt",path+L"\n"+path);
        {WallpaperMedia media;media.start(L"test",true);media.setActive(true);auto until=GetTickCount64()+5000;while(!media.snapshot().image&&GetTickCount64()<until)Sleep(10);check(media.snapshot().image!=nullptr,"initial frame");
            auto previous=media.snapshot().image->id;int changes=0;auto started=GetTickCount64();while(GetTickCount64()-started<1000){Sleep(5);auto s=media.snapshot();check(s.error.empty(),"playback error");if(s.image->id!=previous){changes++;previous=s.image->id;}}std::cout<<"Observed frames/s: "<<changes<<"\n";check(changes>=20&&changes<=35,"playback cadence");
            media.setActive(false);Sleep(100);auto paused=media.snapshot().image->id;Sleep(300);check(media.snapshot().image->id==paused,"occluded pause");media.setActive(true);Sleep(200);check(media.snapshot().image->id!=paused,"resume");
            media.enable(false);Sleep(100);paused=media.snapshot().image->id;Sleep(200);check(media.snapshot().image->id==paused,"disabled pause");media.enable(true);Sleep(2300);check(media.snapshot().error.empty()&&media.snapshot().image->id!=paused,"loop after enable");media.clear();Sleep(100);check(!media.snapshot().image,"clear");media.stop();std::cout<<"Playback changes in 1s: "<<changes<<"; decode, pixels, fallback, loop, pause, resume, disable, clear passed\n";
        }
        DeleteFileW((root+L"\\test-wallpaper-source.txt").c_str());DeleteFileW(path.c_str());DeleteFileW((root+L"\\settings.ini").c_str());RemoveDirectoryW(root.c_str());
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";result=1;}
    MFShutdown();CoUninitialize();return result;
}
