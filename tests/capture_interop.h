// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <dxgi.h>
#include <inspectable.h>
// Narrow declarations of the public Windows SDK interop ABI. The bundled
// MinGW aggregate headers otherwise redefine IReference<BYTE>/boolean.
struct __declspec(uuid("3628e81b-3cac-4c60-b7f4-23ce0e0c3356")) IslandCaptureItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND,REFIID,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR,REFIID,void**)=0;
};
struct __declspec(uuid("a9b3d012-3df2-4ee3-b8d1-8695f457d3c1")) IslandDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID,void**)=0;
};
__CRT_UUID_DECL(IslandCaptureItemInterop,0x3628e81b,0x3cac,0x4c60,0xb7,0xf4,0x23,0xce,0x0e,0x0c,0x33,0x56)
__CRT_UUID_DECL(IslandDxgiInterfaceAccess,0xa9b3d012,0x3df2,0x4ee3,0xb8,0xd1,0x86,0x95,0xf4,0x57,0xd3,0xc1)
extern "C" HRESULT CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*,IInspectable**);
