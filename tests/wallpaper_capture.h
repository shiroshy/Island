// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <d3d10.h>
#include "capture_interop.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
// Test/diagnostic capture of a rendered fixture. The production desktop embeds
// Wallpaper Engine directly and does not use a capture/CPU readback pipeline.
// It never reads pixels back to CPU memory or captures the user's monitor.
class WallpaperGpuCapture {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    WallpaperGpuCapture();
    ~WallpaperGpuCapture();
    bool draw(Graphics& graphics,HWND source,Box destination,int placement);
    void stop();
    HRESULT error()const;
    int stage()const;
    uint64_t frames()const;
    ID3D11Texture2D* frameTexture()const;
};

struct WallpaperGpuCapture::Impl {
    HWND source=nullptr;ID3D11Device* device=nullptr;HRESULT status=S_OK;uint64_t count=0;bool failed=false;int stage=0;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice captureDevice{nullptr};
    Com<ID3D11Texture2D> texture;winrt::Windows::Graphics::SizeInt32 size{};
    void close(){try{if(session)session.Close();if(pool)pool.Close();}catch(...){}session=nullptr;pool=nullptr;item=nullptr;captureDevice=nullptr;texture.reset();}
    ~Impl(){close();}
};
WallpaperGpuCapture::WallpaperGpuCapture()=default;
WallpaperGpuCapture::~WallpaperGpuCapture()=default;
void WallpaperGpuCapture::stop(){impl.reset();}
HRESULT WallpaperGpuCapture::error()const{return impl?impl->status:S_OK;}
int WallpaperGpuCapture::stage()const{return impl?impl->stage:0;}
uint64_t WallpaperGpuCapture::frames()const{return impl?impl->count:0;}
ID3D11Texture2D* WallpaperGpuCapture::frameTexture()const{return impl?impl->texture.p:nullptr;}
bool WallpaperGpuCapture::draw(Graphics& graphics,HWND source,Box destination,int placement){
    if(!source||!IsWindow(source)){stop();return false;}
    auto device=graphics.renderDevice();if(!impl||impl->source!=source||impl->device!=device){impl=std::make_unique<Impl>();impl->source=source;impl->device=device;}
    auto& c=*impl;if(c.failed)return false;
    try{
        namespace Capture=winrt::Windows::Graphics::Capture;namespace DirectX=winrt::Windows::Graphics::DirectX;
        if(!c.session){
            c.stage=1;
            if(!Capture::GraphicsCaptureSession::IsSupported())winrt::throw_hresult(E_NOTIMPL);
            Com<ID3D10Multithread> multithread;if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(multithread.put()))))multithread->SetMultithreadProtected(TRUE);
            c.stage=2;Com<IDXGIDevice> dxgi;winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi.put())));winrt::com_ptr<IInspectable> inspectable;winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.p,inspectable.put()));c.captureDevice=inspectable.as<DirectX::Direct3D11::IDirect3DDevice>();
            c.stage=3;auto factory=winrt::get_activation_factory<Capture::GraphicsCaptureItem,IslandCaptureItemInterop>();winrt::check_hresult(factory->CreateForWindow(source,winrt::guid_of<Capture::GraphicsCaptureItem>(),winrt::put_abi(c.item)));c.stage=31;c.size=c.item.Size();if(c.size.Width<=0||c.size.Height<=0||c.size.Width>16384||c.size.Height>16384)winrt::throw_hresult(E_INVALIDARG);
            c.stage=4;c.pool=Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(c.captureDevice,DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,c.size);c.stage=5;c.session=c.pool.CreateCaptureSession(c.item);c.stage=6;c.session.IsCursorCaptureEnabled(false);c.stage=7;c.session.StartCapture();
        }
        // Drain the bounded pool, keeping only the newest frame.
        c.stage=8;auto frame=c.pool.TryGetNextFrame();for(int i=0;i<2&&frame;i++){auto next=c.pool.TryGetNextFrame();if(!next)break;frame.Close();frame=std::move(next);}
        if(frame){auto actual=frame.ContentSize();if(actual.Width>0&&actual.Height>0&&actual.Width<=16384&&actual.Height<=16384){
                c.stage=9;auto access=frame.Surface().as<IslandDxgiInterfaceAccess>();Com<ID3D11Texture2D> input;winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(input.put())));D3D11_TEXTURE2D_DESC desc{};input->GetDesc(&desc);auto inputWidth=desc.Width,inputHeight=desc.Height;
                if(!c.texture||actual.Width!=c.size.Width||actual.Height!=c.size.Height){c.texture.reset();desc.Width=actual.Width;desc.Height=actual.Height;desc.MipLevels=1;desc.ArraySize=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.MiscFlags=0;winrt::check_hresult(device->CreateTexture2D(&desc,nullptr,c.texture.put()));}
                D3D11_BOX area{0,0,0,(UINT)std::min(actual.Width,(int)inputWidth),(UINT)std::min(actual.Height,(int)inputHeight),1};Com<ID3D11DeviceContext> context;device->GetImmediateContext(context.put());context->CopySubresourceRegion(c.texture.p,0,0,0,0,input.p,0,&area);c.count++;
                bool resized=actual.Width!=c.size.Width||actual.Height!=c.size.Height;frame.Close();if(resized){c.size=actual;c.pool.Recreate(c.captureDevice,DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,c.size);}
            }else frame.Close();}
        if(c.texture){return true;}return false;
    }catch(const winrt::hresult_error& e){c.status=e.code();c.failed=true;c.close();return false;}catch(...){c.status=E_FAIL;c.failed=true;c.close();return false;}
}
