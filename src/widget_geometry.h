// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "graphics.h"
#include <cmath>
inline D2D1_POINT_2F rotateWidgetPoint(D2D1_POINT_2F p,Box box,float degrees){float angle=degrees*3.14159265359f/180,c=std::cos(angle),s=std::sin(angle),cx=box.x+box.w/2,cy=box.y+box.h/2;return {cx+(p.x-cx)*c-(p.y-cy)*s,cy+(p.x-cx)*s+(p.y-cy)*c};}
inline Box rotatedWidgetBounds(Box b,float degrees){float angle=degrees*3.14159265359f/180,c=std::abs(std::cos(angle)),s=std::abs(std::sin(angle)),w=b.w*c+b.h*s,h=b.w*s+b.h*c;return {b.x+(b.w-w)/2,b.y+(b.h-h)/2,w,h};}
inline Box constrainWidget(Box b,float degrees,float width,float height){auto extent=rotatedWidgetBounds(b,degrees);float k=std::min({1.f,(width-24)/extent.w,(height-54)/extent.h});if(k<1){b.w*=k;b.h*=k;extent=rotatedWidgetBounds(b,degrees);}float x=std::clamp(extent.x,12.f,std::max(12.f,width-extent.w-12)),y=std::clamp(extent.y,42.f,std::max(42.f,height-extent.h-12));b.x+=x-extent.x;b.y+=y-extent.y;return b;}
