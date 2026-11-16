#pragma once
#include "graphics.h"
#include <cmath>

namespace welcome_visual {
constexpr float GreetingMilliseconds=1450, SweepMilliseconds=1350;
inline float smooth(float value){value=std::clamp(value,0.f,1.f);return value*value*(3-2*value);}
inline float front(float width,float height,float progress){
    const float slant=height*.24f,tail=width*.18f;
    return width+slant+tail-(width+2*slant+2*tail)*smooth(progress);
}
// Draw only a veil. Its transparent right side exposes the actual live desktop.
inline void draw(Graphics& gfx,float width,float height,float greeting,float sweep,bool revealing){
    const unsigned dark=0x080a10;
    if(!revealing){
        gfx.fill({0,0,width,height},dark,0,1,false);
        float opacity=smooth(greeting/420.f);
        float size=std::clamp(height*.065f,32.f,82.f);
        gfx.layer(opacity,0,12*(1-opacity));
        gfx.text(L"Welcome",{0,height*.5f-size,width,size*2},size,0xf3f3f7,false,DWRITE_TEXT_ALIGNMENT_CENTER,false,L"Segoe UI Light");
        gfx.endLayer();
        return;
    }
    float progress=std::clamp(sweep/SweepMilliseconds,0.f,1.f);
    float edge=front(width,height,progress),slant=height*.24f;
    gfx.polygon({{-slant-2,0},{edge,0},{edge-slant,height},{-slant-2,height}},dark,1);
    // Staggered parallel cuts travel from right to left, followed by clear space.
    float band=std::clamp(width*.022f,18.f,54.f),gap=band*.38f;
    const unsigned colors[]={0x181d28,0x2a3141,0x424d61};
    for(int i=0;i<3;i++){
        float x=edge+gap+i*(band+gap);
        gfx.polygon({{x,0},{x+band,0},{x+band-slant,height},{x-slant,height}},colors[i],.94f-i*.09f);
    }
    float opacity=1-smooth(sweep/260.f);
    if(opacity>0){
        float size=std::clamp(height*.065f,32.f,82.f);
        gfx.layer(opacity,0,-10*(1-opacity));
        gfx.text(L"Welcome",{0,height*.5f-size,width,size*2},size,0xf3f3f7,false,DWRITE_TEXT_ALIGNMENT_CENTER,false,L"Segoe UI Light");
        gfx.endLayer();
    }
}
}
