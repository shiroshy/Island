// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#define NOMINMAX
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <string>
#include <vector>
#include <algorithm>
#include "tasks.h"

template<class T> struct Com {
    T* p = nullptr;
    ~Com() { reset(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
    Com& operator=(Com&& other) noexcept { if (this != &other) { reset(); p=other.p; other.p=nullptr; } return *this; }
    T* operator->() const { return p; }
    operator bool() const { return p != nullptr; }
    T** put() { reset(); return &p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};
inline D2D1_COLOR_F rgb(unsigned c, float a = 1) {
    return D2D1::ColorF((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, a);
}
inline float notchRadius(float height) { return 15.f+7.f*std::clamp((height-36.f)/306.f,0.f,1.f); }
struct Box {
    float x, y, w, h;
    D2D1_RECT_F rect() const { return D2D1::RectF(x, y, x+w, y+h); }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x+w && py < y+h; }
};
struct PictureLayout {Box source,destination;};
inline PictureLayout fitPicture(Box target,float width,float height,int mode){
    PictureLayout r{{0,0,width,height},target};
    if(width<=0||height<=0||target.w<=0||target.h<=0)return r;
    if(mode==0){float factor=std::max(target.w/width,target.h/height);r.source={(width-target.w/factor)/2,(height-target.h/factor)/2,target.w/factor,target.h/factor};}
    if(mode==1){float factor=std::min(target.w/width,target.h/height);r.destination={target.x+(target.w-width*factor)/2,target.y+(target.h-height*factor)/2,width*factor,height*factor};}
    return r;
}
class Graphics {
    Com<ID3D11Device> d3d;
    Com<ID3D11DeviceContext> d3dContext;
    Com<IDXGISwapChain1> swap;
    Com<ID2D1Factory1> factory;
    Com<ID2D1Device> device;
    Com<ID2D1DeviceContext> context;
    Com<ID2D1DeviceContext> contentContext;
    Com<ID2D1CommandList> contentCommands,previousCommands;
    float savedOriginX=0,savedOriginY=0;
    bool recordingContent=false,contentFailed=false;
    Com<ID2D1Bitmap1> target;
    Com<ID2D1SolidColorBrush> brush;
    Com<IDWriteFactory> textFactory;
    Com<IDWriteFontFace> contourFace;
    Com<IDCompositionDevice> composition;
    Com<IDCompositionTarget> compositionTarget;
    Com<IDCompositionVisual> visual;
    Com<ID2D1Bitmap1> cover;
    Com<ID2D1Bitmap1> previous;
    Com<ID2D1Bitmap1> wallpaperBitmap;
    Com<ID2D1Bitmap1> widgetBitmap;
    uint64_t widgetBitmapId=~0ull;
    Com<ID2D1Effect> wallpaperBlur;
    uint64_t wallpaperBitmapId=~0ull;
    struct CachedImage{Com<ID2D1Bitmap1> bitmap;uint64_t used=0;};std::map<uint64_t,CachedImage> appIcons;uint64_t frameSerial=0;
    unsigned long long coverRevision = ~0ull;
    struct TextStyle {
        float size;
        bool bold,wrap;
        DWRITE_TEXT_ALIGNMENT align;
        DWRITE_PARAGRAPH_ALIGNMENT vertical;
        std::wstring family;
        Com<IDWriteTextFormat> format;
    };
    std::vector<TextStyle> styles;
    struct WidthMeasure{std::wstring text,family;float size,width;};std::vector<WidthMeasure> measuredWidths;
    Com<ID2D1PathGeometry> islandPath;
    float originX=0,originY=0;
    float scale = 1;
    UINT pixelWidth=0,pixelHeight=0;
    bool opaqueTarget=false;
    HRESULT lastStatus=S_OK;
public:
    unsigned outlineColor=0x77717f;
    float outlineWidth=0;
    bool outlineSmooth=true;
    bool shadowEnabled=false;
    bool init(HWND hwnd, UINT width, UINT height, float dpiScale, bool opaque=false);
    bool resize(UINT width, UINT height, float dpiScale);
    void begin();
    bool end();
    HRESULT result()const{return lastStatus;}
    bool beginContent();
    void endContent();
    void sprite(const std::shared_ptr<const PixelImage>& image,Box r,int placement=1);
    float textWidth(const std::wstring& value,float size=11,const wchar_t* family=L"Bahnschrift");
    void fill(Box r,unsigned color,float radius=0,float opacity=1,bool outlined=true);
    void stroke(Box r, unsigned color, float radius = 0, float width = 1);
    void line(float x, float y, float x2, float y2, unsigned color, float width = 1);
    void text(const std::wstring& value, Box r, float size = 13, unsigned color = 0xeeeeef,
              bool bold = false, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              bool wrap = false, const wchar_t* family = L"Segoe UI",
              DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void icon(const wchar_t* glyph, Box r, float size = 16, unsigned color = 0x9b999f);
    void notch(float center,float width,float height,unsigned color=0x09090b,const std::shared_ptr<const PixelImage>& image={},float opacity=1,float shade=0,float blur=0);
    void island(Box body,int edge,unsigned color,const std::shared_ptr<const PixelImage>& image={},float opacity=1,float shade=0,float blur=0);
    void clipIsland();
    void unclipIsland();
    void setOrigin(float x,float y){originX=x;originY=y;context->SetTransform(D2D1::Matrix3x2F::Translation(x,y));}
    D2D1_MATRIX_3X2_F transform()const{D2D1_MATRIX_3X2_F m;context->GetTransform(&m);return m;}
    void transform(D2D1_MATRIX_3X2_F m){context->SetTransform(m);}
    void textOutline(const std::wstring& value,Box r,float size,unsigned color,float thickness=1.5f);
    void clip(Box r);
    void clipRounded(Box r,float radius);
    void unclipRounded();
    void unclip();
    void image(const std::vector<unsigned char>& data, unsigned long long revision, Box r);
    void pixels(const std::shared_ptr<const PixelImage>& image,Box r);
    void picture(const std::shared_ptr<const PixelImage>& image,Box r,int placement=0,float opacity=1,float blur=0);
    void releaseWallpaper(){wallpaperBlur.reset();wallpaperBitmap.reset();wallpaperBitmapId=~0ull;}
    void gradient(Box r,unsigned first,unsigned last,bool vertical=true,float opacity=1,float finalOpacity=-1);
    void glow(Box r,unsigned color,float opacity);
    void ribbon(Box r,unsigned color,float opacity);
    void snapshot();
    void previousFrame(float opacity,float offsetX=0);
    void layer(float opacity,float offsetX=0,float offsetY=0);
    void endLayer();
    void releaseSnapshot() { previous.reset();previousCommands.reset(); }
};
