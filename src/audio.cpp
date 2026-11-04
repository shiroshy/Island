// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "graphics.h"
#include "audio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <cmath>
#include <chrono>

bool SpectrumAnalyzer::feed(float sample){samples[cursor]=std::isfinite(sample)?std::clamp(sample,-1.f,1.f):0;cursor=(cursor+1)%samples.size();return ++total>=samples.size()&&total%1024==0;}
SpectrumFrame SpectrumAnalyzer::calculate(unsigned sampleRate)const{
    constexpr size_t N=4096;constexpr float pi=3.14159265359f;SpectrumFrame output;std::array<std::complex<float>,N> fft{};
    for(size_t i=0;i<N;i++){float value=samples[(cursor+i)%N];output.rms+=value*value;fft[i]=value*(.5f-.5f*std::cos(2*pi*i/(N-1)));}
    output.rms=std::sqrt(output.rms/N);
    for(size_t i=1,j=0;i<N;i++){size_t bit=N>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(fft[i],fft[j]);}
    for(size_t len=2;len<=N;len<<=1){std::complex<float> step=std::polar(1.f,-2*pi/len);for(size_t i=0;i<N;i+=len){std::complex<float> w=1;for(size_t j=0;j<len/2;j++){auto a=fft[i+j],b=fft[i+j+len/2]*w;fft[i+j]=a+b;fft[i+j+len/2]=a-b;w*=step;}}}
    float high=std::min(18000.f,sampleRate*.48f);
    for(size_t band=0;band<output.bars.size();band++){
        float low=32*std::pow(high/32,band/48.f),upper=32*std::pow(high/32,(band+1)/48.f);int first=std::clamp((int)std::floor(low*N/sampleRate),1,(int)N/2-1),last=std::clamp((int)std::ceil(upper*N/sampleRate),first+1,(int)N/2);
        float magnitude=0;for(int bin=first;bin<last;bin++)magnitude=std::max(magnitude,std::abs(fft[bin])*4/N);
        output.bars[band]=magnitude<.0001f?0:std::clamp((20*std::log10(magnitude)+72)/72,0.f,1.f);
    }
    for(size_t i=0;i<128;i++)output.wave[i]=samples[(cursor+N-1024+i*8)%N];return output;
}



void AudioMeter::run(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    while(!stopping){
        {std::unique_lock lock(mutex);wake.wait(lock,[this]{return stopping||active;});}
        if(stopping)break;
        Com<IMMDeviceEnumerator> enumerator;Com<IMMDevice> output;Com<IAudioClient> client;Com<IAudioCaptureClient> capture;
        WAVEFORMATEX* format=nullptr;
        HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)enumerator.put());
        if(SUCCEEDED(hr))hr=enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,output.put());
        if(SUCCEEDED(hr))hr=output->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)client.put());
        if(SUCCEEDED(hr))hr=client->GetMixFormat(&format);
        if(SUCCEEDED(hr))hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,1000000,0,format,nullptr);
        if(SUCCEEDED(hr))hr=client->GetService(__uuidof(IAudioCaptureClient),(void**)capture.put());
        if(SUCCEEDED(hr))hr=client->Start();
        auto currentGeneration=generation.load();ULONGLONG deviceCheck=GetTickCount64();LPWSTR currentId=nullptr;if(output)output->GetId(&currentId);
        float low=0,mid=0;SpectrumAnalyzer analyzer;ULONGLONG lastSamples=GetTickCount64(),lastAnalysis=0;
        if(SUCCEEDED(hr)){
            float aLow=1-std::exp(-2*3.14159265f*220/format->nSamplesPerSec);
            float aMid=1-std::exp(-2*3.14159265f*2200/format->nSamplesPerSec);
            bool floating=format->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
            if(format->wFormatTag==WAVE_FORMAT_EXTENSIBLE)floating=((WAVEFORMATEXTENSIBLE*)format)->SubFormat.Data1==WAVE_FORMAT_IEEE_FLOAT;
            while(active&&!stopping&&generation==currentGeneration){
                if(GetTickCount64()-deviceCheck>=2000){deviceCheck=GetTickCount64();Com<IMMDevice> latest;LPWSTR id=nullptr;bool changed=FAILED(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,latest.put()));if(!changed){latest->GetId(&id);changed=!id||!currentId||wcscmp(id,currentId);}if(id)CoTaskMemFree(id);if(changed)break;}
                UINT32 packet=0;hr=capture->GetNextPacketSize(&packet);if(FAILED(hr))break;
                double energy[3]={};unsigned count=0;
                while(packet&&active&&!stopping&&generation==currentGeneration){
                    BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;
                    hr=capture->GetBuffer(&data,&frames,&flags,nullptr,nullptr);if(FAILED(hr))break;
                    for(UINT32 f=0;f<frames;f++){
                        float sample=0;
                        if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT)&&data){
                            for(WORD ch=0;ch<format->nChannels;ch++){
                                const BYTE* p=data+f*format->nBlockAlign+ch*(format->wBitsPerSample/8);
                                if(floating&&format->wBitsPerSample==32)sample+=*(const float*)p;
                                else if(format->wBitsPerSample==16)sample+=*(const short*)p/32768.f;
                                else if(format->wBitsPerSample==32)sample+=*(const int*)p/2147483648.f;
                            }
                            sample/=std::max<WORD>(1,format->nChannels);
                        }
                        low+=aLow*(sample-low);mid+=aMid*(sample-mid);
                        if(detailed&&analyzer.feed(sample)&&GetTickCount64()-lastAnalysis>=(ULONGLONG)(1000/analysisRate.load())){lastAnalysis=GetTickCount64();auto frame=analyzer.calculate(format->nSamplesPerSec);std::lock_guard lock(spectrumMutex);spectrumFrame=frame;}
                        float parts[]={low,mid-low,sample-mid};
                        for(int i=0;i<3;i++)energy[i]+=parts[i]*parts[i];
                        ++count;
                    }
                    capture->ReleaseBuffer(frames);hr=capture->GetNextPacketSize(&packet);if(FAILED(hr))break;
                }
                for(int i=0;i<3;i++){
                    float rms=count?(float)std::sqrt(energy[i]/count):0;
                    bands[i]=rms<.0001f?0.f:std::clamp(std::sqrt(rms)*2.1f,0.f,1.f);
                }
                if(count)lastSamples=GetTickCount64();else if(GetTickCount64()-lastSamples>180){std::lock_guard lock(spectrumMutex);spectrumFrame={};}
                std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(12),[this]{return stopping||!active;});
            }
            client->Stop();
        }
        if(currentId)CoTaskMemFree(currentId);
        if(format)CoTaskMemFree(format);
        for(auto& band:bands)band=0;
        {std::lock_guard lock(spectrumMutex);spectrumFrame={};}
        if(active&&!stopping){std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::seconds(1),[this]{return stopping||!active;});}
    }
    CoUninitialize();
}
