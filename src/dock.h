// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "graphics.h"
#include <array>
#include <cmath>
struct DockPoint{float x=0,y=0;};
inline DockPoint edgePoint(Box b,int edge,float u,float v){if(edge==1)return {b.x+b.w-v,b.y+u};if(edge==2)return {b.x+u,b.y+b.h-v};if(edge==3)return {b.x+v,b.y+u};return {b.x+u,b.y+v};}
inline std::array<DockPoint,16> islandOutline(Box b,int edge){
    bool side=edge==1||edge==3;float a=side?b.h:b.w,d=side?b.w:b.h,r=notchRadius(d);
    std::array<DockPoint,16> p={{{a+12,0},{a+3,0},{a,3},{a,12},{a,d-r},{a,d-r/3},{a-r/3,d},{a-r,d},{r,d},{r/3,d},{0,d-r/3},{0,d-r},{0,12},{0,3},{-3,0},{-12,0}}};
    for(auto& q:p)q=edgePoint(b,edge,q.x,q.y);return p;
}
inline Box dockBody(int edge,float w,float h,float surfaceW=736,float surfaceH=476){
    if(edge==1||edge==3){float t=std::clamp((h-36)/306,0.f,1.f),depth=36+660*std::clamp((w-240)/456,0.f,1.f),along=240+102*t;return {edge==3?0:surfaceW-depth,(surfaceH-along)/2,depth,along};}
    return {(surfaceW-w)/2,edge==2?surfaceH-h:0,w,h};
}
inline float snapDockRatio(float ratio){float distance=std::abs(ratio-.5f);if(distance<.018f)return .5f;if(distance<.065f){float t=(distance-.018f)/.047f;t=t*t*(3-2*t);return .5f+(ratio-.5f)*t;}return ratio;}
