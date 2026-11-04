// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "graphics.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <d2d1effects.h>
#include "dock.h"
#include <cmath>

bool Graphics::init(HWND hwnd, UINT width, UINT height, float dpiScale,bool opaque) {
    auto check=[this](HRESULT value){lastStatus=value;return SUCCEEDED(value);};
    opaqueTarget=opaque;
    scale = dpiScale;
    pixelWidth=width;pixelHeight=height;
    D3D_FEATURE_LEVEL feature;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), &feature, d3dContext.put());
    if (!check(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), &feature, d3dContext.put());
    if (!check(hr)) return false;
    Com<IDXGIDevice> dxgi;
    if (!check(d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgi.put()))) return false;
    Com<IDXGIDevice1> dxgiLatency;
    if(SUCCEEDED(dxgi->QueryInterface(__uuidof(IDXGIDevice1),(void**)dxgiLatency.put())))dxgiLatency->SetMaximumFrameLatency(1);
    D2D1_FACTORY_OPTIONS options = {};
    if (!check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, (void**)factory.put()))) return false;
    if (!check(factory->CreateDevice(dxgi.p, device.put())) || !check(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.put()))) return false;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory2> dxgiFactory;
    if (!check(dxgi->GetAdapter(adapter.put())) || !check(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)dxgiFactory.put()))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width; desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = opaque?DXGI_ALPHA_MODE_IGNORE:DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (opaque){if(!check(dxgiFactory->CreateSwapChainForHwnd(d3d.p,hwnd,&desc,nullptr,nullptr,swap.put())))return false;}
    else if (!check(dxgiFactory->CreateSwapChainForComposition(d3d.p, &desc, nullptr, swap.put()))) return false;
    Com<IDXGISurface> surface;
    if (!check(swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)surface.put()))) return false;
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat = D2D1::PixelFormat(desc.Format, opaque?D2D1_ALPHA_MODE_IGNORE:D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = props.dpiY = 96 * scale;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (!check(context->CreateBitmapFromDxgiSurface(surface.p, &props, target.put()))) return false;
    context->SetTarget(target.p);
    context->SetDpi(96*scale, 96*scale);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (!check(context->CreateSolidColorBrush(rgb(0xffffff), brush.put()))) return false;
    if (!check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)textFactory.put()))) return false;
    if(opaque)return true;
    if (!check(DCompositionCreateDevice(dxgi.p, __uuidof(IDCompositionDevice), (void**)composition.put()))) return false;
    // Keep native text fields above the composition visual.
    if (!check(composition->CreateTargetForHwnd(hwnd, FALSE, compositionTarget.put()))) return false;
    if (!check(composition->CreateVisual(visual.put()))) return false;
    visual->SetContent(swap.p);
    compositionTarget->SetRoot(visual.p);
    return check(composition->Commit());
}
bool Graphics::resize(UINT width,UINT height,float dpiScale) {
    auto check=[this](HRESULT value){lastStatus=value;return SUCCEEDED(value);};
    if(width==pixelWidth&&height==pixelHeight&&dpiScale==scale)return true;
    contentCommands.reset();previousCommands.reset();contentContext.reset();
    context->Flush();context->SetTarget(nullptr);target.reset();previous.reset();context->Flush();
    brush.reset();cover.reset();coverRevision=~0ull;wallpaperBlur.reset();wallpaperBitmap.reset();wallpaperBitmapId=~0ull;widgetBitmap.reset();widgetBitmapId=~0ull;appIcons.clear();context.reset();device->ClearResources(0);
    d3dContext->ClearState();d3dContext->Flush();
    if(!check(swap->ResizeBuffers(2,width,height,DXGI_FORMAT_B8G8R8A8_UNORM,0)))return false;
    if(!check(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put())))return false;
    scale=dpiScale;pixelWidth=width;pixelHeight=height;
    Com<IDXGISurface> surface;
    if(!check(swap->GetBuffer(0,__uuidof(IDXGISurface),(void**)surface.put())))return false;
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,opaqueTarget?D2D1_ALPHA_MODE_IGNORE:D2D1_ALPHA_MODE_PREMULTIPLIED),96*scale,96*scale);
    if(!check(context->CreateBitmapFromDxgiSurface(surface.p,&props,target.put())))return false;
    context->SetTarget(target.p);context->SetDpi(96*scale,96*scale);context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if(!check(context->CreateSolidColorBrush(rgb(0xffffff),brush.put())))return false;
    return true;
}
void Graphics::begin() {
    contentFailed=false;++frameSerial;context->BeginDraw();
    originX=originY=0;
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->Clear(D2D1::ColorF(0, 0.f));
}
bool Graphics::end() {
    HRESULT hr = context->EndDraw();lastStatus=hr;
    if (FAILED(hr)||contentFailed) return false;
    lastStatus=swap->Present(0, 0);return SUCCEEDED(lastStatus);
}
float Graphics::textWidth(const std::wstring& value,float size,const wchar_t* family){
    for(auto& measured:measuredWidths)if(measured.size==size&&measured.text==value&&measured.family==family)return measured.width;
    Com<IDWriteTextFormat> format;Com<IDWriteTextLayout> layout;
    if(FAILED(textFactory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"ru-RU",format.put())))return (float)value.size()*size*.65f;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if(FAILED(textFactory->CreateTextLayout(value.c_str(),(UINT32)value.size(),format.p,4096,100,layout.put())))return (float)value.size()*size*.65f;
    DWRITE_TEXT_METRICS metrics{};layout->GetMetrics(&metrics);float width=std::ceil(metrics.widthIncludingTrailingWhitespace);if(measuredWidths.size()<128)measuredWidths.push_back({value,family,size,width});return width;
}
void Graphics::fill(Box r,unsigned color,float radius,float opacity,bool outlined) {
    if(shadowEnabled){brush->SetColor(rgb(0,.12f*opacity));for(float d:{1.f,3.f,5.f}){auto b=Box{r.x+d,r.y+d+2,r.w,r.h};if(radius>0)context->FillRoundedRectangle(D2D1::RoundedRect(b.rect(),radius,radius),brush.p);else context->FillRectangle(b.rect(),brush.p);}}
    brush->SetColor(rgb(color, opacity));
    if (radius > 0) context->FillRoundedRectangle(D2D1::RoundedRect(r.rect(), radius, radius), brush.p);
    else context->FillRectangle(r.rect(), brush.p);
    if(outlined&&outlineWidth>0&&radius>=5&&opacity>=.65f&&r.w>20&&r.h>20){auto mode=context->GetAntialiasMode();context->SetAntialiasMode(outlineSmooth?D2D1_ANTIALIAS_MODE_PER_PRIMITIVE:D2D1_ANTIALIAS_MODE_ALIASED);float inset=outlineWidth/2;stroke({r.x+inset,r.y+inset,r.w-outlineWidth,r.h-outlineWidth},outlineColor,std::max(1.f,radius-inset),outlineWidth);context->SetAntialiasMode(mode);}
}
void Graphics::stroke(Box r, unsigned color, float radius, float width) {
    brush->SetColor(rgb(color));
    if (radius > 0) context->DrawRoundedRectangle(D2D1::RoundedRect(r.rect(), radius, radius), brush.p, width);
    else context->DrawRectangle(r.rect(), brush.p, width);
}
void Graphics::line(float x, float y, float x2, float y2, unsigned color, float width) {
    if(shadowEnabled){brush->SetColor(rgb(0,.10f));for(float d:{1.f,3.f,5.f})context->DrawLine(D2D1::Point2F(x+d,y+d+2),D2D1::Point2F(x2+d,y2+d+2),brush.p,width+2);}
    brush->SetColor(rgb(color));
    context->DrawLine(D2D1::Point2F(x,y), D2D1::Point2F(x2,y2), brush.p, width);
}
void Graphics::text(const std::wstring& value, Box r, float size, unsigned color, bool bold, DWRITE_TEXT_ALIGNMENT align, bool wrap, const wchar_t* family, DWRITE_PARAGRAPH_ALIGNMENT vertical) {
    IDWriteTextFormat* fmt=nullptr;
    for(auto& s:styles)if(s.size==size&&s.bold==bold&&s.wrap==wrap&&s.align==align&&s.vertical==vertical&&s.family==family){fmt=s.format.p;break;}
    if(!fmt){
        TextStyle style{size,bold,wrap,align,vertical,family,{}};
        if (FAILED(textFactory->CreateTextFormat(family, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", style.format.put()))) return;
        fmt=style.format.p;
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(vertical);
        fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {};trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        Com<IDWriteInlineObject> ellipsis;textFactory->CreateEllipsisTrimmingSign(fmt,ellipsis.put());fmt->SetTrimming(&trimming,ellipsis.p);
        styles.push_back(std::move(style));
    }
    if(shadowEnabled){brush->SetColor(rgb(0,.075f));for(float dx:{-2.f,0.f,2.f})for(float dy:{1.f,3.f,5.f})context->DrawTextW(value.c_str(),(UINT32)value.size(),fmt,Box{r.x+dx+2,r.y+dy+2,r.w,r.h}.rect(),brush.p,D2D1_DRAW_TEXT_OPTIONS_CLIP);}
    brush->SetColor(rgb(color));
    context->DrawTextW(value.c_str(), (UINT32)value.size(), fmt, r.rect(), brush.p, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Graphics::icon(const wchar_t* glyph, Box r, float size, unsigned color) {
    text(glyph, r, size, color, false, DWRITE_TEXT_ALIGNMENT_CENTER, false, L"Segoe MDL2 Assets");
}
void Graphics::notch(float center,float width,float height,unsigned color,const std::shared_ptr<const PixelImage>& image,float opacity,float shade,float blur){island({center-width/2,0,width,height},0,color,image,opacity,shade,blur);}
void Graphics::island(Box body,int edge,unsigned color,const std::shared_ptr<const PixelImage>& image,float opacity,float shade,float blur){
    auto points=islandOutline(body,edge);
    auto make=[&](bool closed,ID2D1PathGeometry** result){factory->CreatePathGeometry(result);Com<ID2D1GeometrySink> sink;(*result)->Open(sink.put());auto pt=[&](int i){return D2D1::Point2F(points[i].x,points[i].y);};sink->BeginFigure(pt(0),D2D1_FIGURE_BEGIN_FILLED);sink->AddBezier(D2D1::BezierSegment(pt(1),pt(2),pt(3)));sink->AddLine(pt(4));sink->AddBezier(D2D1::BezierSegment(pt(5),pt(6),pt(7)));sink->AddLine(pt(8));sink->AddBezier(D2D1::BezierSegment(pt(9),pt(10),pt(11)));sink->AddLine(pt(12));sink->AddBezier(D2D1::BezierSegment(pt(13),pt(14),pt(15)));sink->EndFigure(closed?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);sink->Close();};
    islandPath.reset();make(true,islandPath.put());brush->SetColor(rgb(color));context->FillGeometry(islandPath.p,brush.p);
    if(image){clipIsland();picture(image,{body.x-12,body.y-12,body.w+24,body.h+24},0,opacity,blur);fill({body.x-12,body.y-12,body.w+24,body.h+24},0,0,shade);unclipIsland();}
    if(outlineWidth>0){Com<ID2D1PathGeometry> outline;make(false,outline.put());auto mode=context->GetAntialiasMode();context->SetAntialiasMode(outlineSmooth?D2D1_ANTIALIAS_MODE_PER_PRIMITIVE:D2D1_ANTIALIAS_MODE_ALIASED);brush->SetColor(rgb(outlineColor));context->DrawGeometry(outline.p,brush.p,outlineWidth);context->SetAntialiasMode(mode);}
}
void Graphics::clipIsland(){auto mask=D2D1::LayerParameters1();mask.geometricMask=islandPath.p;context->PushLayer(mask,nullptr);}
void Graphics::unclipIsland(){context->PopLayer();}
void Graphics::textOutline(const std::wstring& value,Box r,float size,unsigned color,float thickness){
    if(!contourFace){Com<IDWriteFontCollection> fonts;Com<IDWriteFontFamily> family;Com<IDWriteFont> font;UINT32 index=0;BOOL exists=FALSE;textFactory->GetSystemFontCollection(fonts.put());fonts->FindFamilyName(L"Bahnschrift",&index,&exists);if(!exists)index=0;if(FAILED(fonts->GetFontFamily(index,family.put()))||FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,font.put()))||FAILED(font->CreateFontFace(contourFace.put())))return;}
    std::vector<UINT32> code(value.begin(),value.end());std::vector<UINT16> glyphs(value.size());std::vector<DWRITE_GLYPH_METRICS> gm(value.size());std::vector<FLOAT> advances(value.size());contourFace->GetGlyphIndices(code.data(),(UINT32)code.size(),glyphs.data());contourFace->GetDesignGlyphMetrics(glyphs.data(),(UINT32)glyphs.size(),gm.data());DWRITE_FONT_METRICS metrics{};contourFace->GetMetrics(&metrics);float unit=size/metrics.designUnitsPerEm;for(size_t i=0;i<gm.size();i++)advances[i]=gm[i].advanceWidth*unit;
    Com<ID2D1PathGeometry> path;Com<ID2D1GeometrySink> sink;factory->CreatePathGeometry(path.put());path->Open(sink.put());contourFace->GetGlyphRunOutline(size,glyphs.data(),advances.data(),nullptr,(UINT32)glyphs.size(),FALSE,FALSE,sink.p);sink->Close();D2D1_MATRIX_3X2_F previous;context->GetTransform(&previous);context->SetTransform(D2D1::Matrix3x2F::Translation(r.x,r.y+(r.h-(metrics.ascent+metrics.descent)*unit)/2+metrics.ascent*unit)*previous);if(shadowEnabled){D2D1_MATRIX_3X2_F original;context->GetTransform(&original);brush->SetColor(rgb(0,.14f));for(float d:{1.f,3.f,5.f}){context->SetTransform(D2D1::Matrix3x2F::Translation(d,d+2)*original);context->DrawGeometry(path.p,brush.p,thickness+2);}context->SetTransform(original);}brush->SetColor(rgb(color));context->DrawGeometry(path.p,brush.p,thickness);context->SetTransform(previous);
}
void Graphics::clip(Box r) { context->PushAxisAlignedClip(r.rect(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
void Graphics::unclip() { context->PopAxisAlignedClip(); }
void Graphics::image(const std::vector<unsigned char>& data, unsigned long long revision, Box r) {
    if (revision != coverRevision) {
        coverRevision=revision; cover.reset();
        if (!data.empty()) {
            Com<IWICImagingFactory> wic;
            Com<IStream> stream;
            Com<IWICBitmapDecoder> decoder;
            Com<IWICBitmapFrameDecode> frame;
            Com<IWICFormatConverter> converter;
            stream.p=SHCreateMemStream(data.data(),(UINT)data.size());
            if (stream && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IWICImagingFactory),(void**)wic.put())) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.p,nullptr,WICDecodeMetadataCacheOnLoad,decoder.put())) &&
                SUCCEEDED(decoder->GetFrame(0,frame.put())) && SUCCEEDED(wic->CreateFormatConverter(converter.put())) &&
                SUCCEEDED(converter->Initialize(frame.p,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
            {
                auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
                context->CreateBitmapFromWicBitmap(converter.p,&props,cover.put());
            }
        }
    }
    if (cover) {
        auto size=cover->GetSize();float fit=std::min(r.w/size.width,r.h/size.height);
        Box dest{r.x+(r.w-size.width*fit)*.5f,r.y+(r.h-size.height*fit)*.5f,size.width*fit,size.height*fit};
        auto rect=dest.rect();context->DrawBitmap(cover.p,&rect,1,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr,nullptr);
    }
}
void Graphics::pixels(const std::shared_ptr<const PixelImage>& image,Box r) {
    picture(image,r,2);
}
void Graphics::picture(const std::shared_ptr<const PixelImage>& image,Box r,int placement,float opacity,float blur) {
    if(!image||image->bgra.empty())return;
    if(image->id>=(1ull<<60)){
        if(wallpaperBitmapId!=image->id||!wallpaperBitmap){
            auto size=wallpaperBitmap?wallpaperBitmap->GetPixelSize():D2D1::SizeU(0,0);
            if(size.width==image->width&&size.height==image->height){if(FAILED(wallpaperBitmap->CopyFromMemory(nullptr,image->bgra.data(),image->width*4)))return;}
            else {releaseWallpaper();auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);if(!wallpaperBitmap&&FAILED(context->CreateBitmap(D2D1::SizeU(image->width,image->height),image->bgra.data(),image->width*4,&props,wallpaperBitmap.put())))return;}wallpaperBitmapId=image->id;
        }
        auto layout=fitPicture(r,(float)image->width,(float)image->height,placement);auto destination=layout.destination.rect(),source=layout.source.rect();
        if(blur>0){if(!wallpaperBlur)context->CreateEffect(CLSID_D2D1GaussianBlur,wallpaperBlur.put());if(wallpaperBlur){wallpaperBlur->SetInput(0,wallpaperBitmap.p);float sx=layout.destination.w/layout.source.w,sy=layout.destination.h/layout.source.h;wallpaperBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,blur/std::max(.01f,std::min(sx,sy)));wallpaperBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD);D2D1_MATRIX_3X2_F old;context->GetTransform(&old);auto params=D2D1::LayerParameters1();params.opacity=opacity;context->PushLayer(params,nullptr);context->SetTransform(D2D1::Matrix3x2F::Scale(sx,sy)*D2D1::Matrix3x2F::Translation(layout.destination.x,layout.destination.y)*old);context->DrawImage(wallpaperBlur.p,D2D1::Point2F(0,0),source,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);context->SetTransform(old);context->PopLayer();return;}}
        context->DrawBitmap(wallpaperBitmap.p,&destination,opacity,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,&source,nullptr);return;
    }
    auto found=appIcons.find(image->id);
    if(found==appIcons.end()){
        if(appIcons.size()>=192){auto oldest=std::min_element(appIcons.begin(),appIcons.end(),[](auto& a,auto& b){return a.second.used<b.second.used;});appIcons.erase(oldest);}
        auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        Com<ID2D1Bitmap1> bitmap;
        if(FAILED(context->CreateBitmap(D2D1::SizeU(image->width,image->height),image->bgra.data(),image->width*4,&props,bitmap.put())))return;
        found=appIcons.emplace(image->id,CachedImage{std::move(bitmap),frameSerial}).first;
    }
    found->second.used=frameSerial;auto layout=fitPicture(r,(float)image->width,(float)image->height,placement);auto destination=layout.destination.rect(),source=layout.source.rect();context->DrawBitmap(found->second.bitmap.p,&destination,opacity,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,&source,nullptr);
}
void Graphics::gradient(Box r,unsigned first,unsigned last,bool vertical,float opacity,float finalOpacity){
    D2D1_GRADIENT_STOP stops[]={{0,rgb(first,opacity)},{1,rgb(last,finalOpacity<0?opacity:finalOpacity)}};Com<ID2D1GradientStopCollection> collection;Com<ID2D1LinearGradientBrush> gradient;
    if(SUCCEEDED(context->CreateGradientStopCollection(stops,2,collection.put()))&&SUCCEEDED(context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.x,r.y},{vertical?r.x:r.x+r.w,vertical?r.y+r.h:r.y}),collection.p,gradient.put())))context->FillRectangle(r.rect(),gradient.p);
}
void Graphics::glow(Box r,unsigned color,float opacity){
    D2D1_GRADIENT_STOP stops[]={{0,rgb(color,opacity)},{1,rgb(color,0)}};Com<ID2D1GradientStopCollection> collection;Com<ID2D1RadialGradientBrush> gradient;
    if(SUCCEEDED(context->CreateGradientStopCollection(stops,2,collection.put()))&&SUCCEEDED(context->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({r.x+r.w/2,r.y+r.h/2},{0,0},r.w/2,r.h/2),collection.p,gradient.put())))context->FillRectangle(r.rect(),gradient.p);
}
void Graphics::ribbon(Box r,unsigned color,float opacity){
    Com<ID2D1PathGeometry> path;Com<ID2D1GeometrySink> sink;if(FAILED(factory->CreatePathGeometry(path.put()))||FAILED(path->Open(sink.put())))return;
    auto point=[&](float x,float y){return D2D1::Point2F(r.x+x*r.w,r.y+y*r.h);};
    sink->BeginFigure(point(-.2f,.92f),D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(point(.1f,.38f),point(.44f,1.37f),point(.72f,.45f)));
    sink->AddBezier(D2D1::BezierSegment(point(.82f,.08f),point(.95f,.15f),point(1.2f,-.16f)));
    sink->AddLine(point(1.2f,.05f));sink->AddBezier(D2D1::BezierSegment(point(.98f,.38f),point(.88f,.26f),point(.79f,.66f)));
    sink->AddBezier(D2D1::BezierSegment(point(.5f,1.49f),point(.07f,.7f),point(-.2f,1.1f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);sink->Close();brush->SetColor(rgb(color,opacity));context->FillGeometry(path.p,brush.p);
}
bool Graphics::beginContent(){
    if(!contentContext){if(FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,contentContext.put())))return false;contentContext->SetDpi(96*scale,96*scale);contentContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);}
    Com<ID2D1CommandList> next;if(FAILED(contentContext->CreateCommandList(next.put())))return false;
    contentCommands=std::move(next);contentContext->SetTarget(contentCommands.p);std::swap(context,contentContext);savedOriginX=originX;savedOriginY=originY;originX=originY=0;context->BeginDraw();context->SetTransform(D2D1::Matrix3x2F::Identity());recordingContent=true;return true;
}
void Graphics::endContent(){if(!recordingContent)return;HRESULT hr=context->EndDraw();context->SetTarget(nullptr);HRESULT close=contentCommands->Close();std::swap(context,contentContext);originX=savedOriginX;originY=savedOriginY;recordingContent=false;if(SUCCEEDED(hr)&&SUCCEEDED(close))context->DrawImage(contentCommands.p);else{contentCommands.reset();contentFailed=true;}}
void Graphics::snapshot(){previousCommands.reset();if(contentCommands){previousCommands.p=contentCommands.p;previousCommands.p->AddRef();}}
void Graphics::previousFrame(float opacity,float offsetX){if(!previousCommands)return;auto params=D2D1::LayerParameters1();params.opacity=opacity;D2D1_MATRIX_3X2_F old;context->GetTransform(&old);context->PushLayer(params,nullptr);context->SetTransform(D2D1::Matrix3x2F::Translation(offsetX,0)*old);context->DrawImage(previousCommands.p);context->SetTransform(old);context->PopLayer();}
void Graphics::clipRounded(Box r,float radius){Com<ID2D1RoundedRectangleGeometry> geometry;factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r.rect(),radius,radius),geometry.put());auto params=D2D1::LayerParameters1();params.geometricMask=geometry.p;context->PushLayer(params,nullptr);}
void Graphics::unclipRounded(){context->PopLayer();}
void Graphics::sprite(const std::shared_ptr<const PixelImage>& image,Box r,int placement){
    if(!image||image->bgra.empty())return;if(widgetBitmapId!=image->id){bool same=widgetBitmap&&widgetBitmap->GetPixelSize().width==image->width&&widgetBitmap->GetPixelSize().height==image->height;if(same){if(FAILED(widgetBitmap->CopyFromMemory(nullptr,image->bgra.data(),image->width*4)))widgetBitmap.reset();}else widgetBitmap.reset();if(!widgetBitmap){auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);if(FAILED(context->CreateBitmap(D2D1::SizeU(image->width,image->height),image->bgra.data(),image->width*4,&props,widgetBitmap.put())))return;}widgetBitmapId=image->id;}
    auto layout=fitPicture(r,(float)image->width,(float)image->height,placement);auto source=layout.source.rect(),destination=layout.destination.rect();context->DrawBitmap(widgetBitmap.p,&destination,1,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,&source,nullptr);
}
void Graphics::layer(float opacity,float offsetX,float offsetY) {
    auto params=D2D1::LayerParameters1();params.opacity=opacity;
    context->PushLayer(params,nullptr);context->SetTransform(D2D1::Matrix3x2F::Translation(originX+offsetX,originY+offsetY));
}
void Graphics::endLayer() { context->SetTransform(D2D1::Matrix3x2F::Translation(originX,originY));context->PopLayer(); }
