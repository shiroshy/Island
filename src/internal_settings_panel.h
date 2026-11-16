// Members of App; compiled only for the settings-inside-Island distribution.
    int internalSection=0,internalOffset=0;bool internalClock=false,internalDragging=false;
    static constexpr int InternalVisibleRows=6;
    island_settings::Id internalSlideId{};double internalSlideValue=0;
    using SettingId=island_settings::Id;
    std::vector<SettingId> internalRows()const{
        using I=SettingId;
        if(internalSection==0)return {I::Startup,I::Hover,I::ShowSeconds,I::DesktopEnabled,I::DockLocked,I::WindowsFileManager,I::Size};
        if(internalSection==1)return {I::Background,I::Card,I::Accent,I::Text,I::Secondary,I::HoverColor,I::Outline,I::OutlineWidth,I::OutlineSmooth,I::ThemeDark,I::PanelPhoto,I::PanelVideo,I::PanelClear,I::PanelOpacity,I::PanelShade,I::PanelBlur};
        if(internalSection==2)return {I::DesktopBackground,I::DesktopCard,I::DesktopAccent,I::DesktopText,I::DesktopSecondary,I::DesktopHoverColor,I::DesktopOutline,I::DesktopOutlineWidth,I::DesktopOutlineSmooth,I::DesktopThemeDark,I::ClockStyle,I::ClockShadow,I::DesktopBackdrop,I::DesktopPhoto,I::DesktopVideo,I::DesktopClear,I::DesktopEngineCurrent,I::DesktopEngine,I::DesktopEngineOff,I::DesktopFit,I::DesktopShade};
        return {I::VideoEnabled,I::PanelFPS,I::DesktopFPS,I::VideoFPS,I::AnalysisFPS};
    }
    static std::vector<std::wstring> internalSplit(const wchar_t* text){std::vector<std::wstring> result;std::wstring value=text;size_t first=0;while(first<value.size()){auto end=value.find(L'|',first);result.push_back(value.substr(first,end==std::wstring::npos?end:end-first));if(end==std::wstring::npos)break;first=end+1;}return result;}
    double internalValue(SettingId id){
        using I=SettingId;auto d=island_settings::find(id);if(!d)return 0;
        if(id==I::Startup)return isStartup();
        if(id==I::DesktopMonitor)return island_settings::settingsMonitorIndex(ini,hwnd);
        if(id==I::ClockShadow){
            std::vector<std::wstring> devices;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR m,HDC,LPRECT,LPARAM p)->BOOL{MONITORINFOEXW info{};info.cbSize=sizeof(info);if(GetMonitorInfoW(m,&info))((std::vector<std::wstring>*)p)->emplace_back(info.szDevice);return TRUE;},(LPARAM)&devices);
            if(devices.empty())return 0;auto device=devices[std::clamp((int)internalValue(I::DesktopMonitor),0,(int)devices.size()-1)];for(auto& c:device)if(!iswalnum(c))c=L'_';wchar_t value[384]={};GetPrivateProfileStringW((L"desktop-layout-"+device).c_str(),L"2",L"",value,384,ini.c_str());std::wstring line=value;std::replace(line.begin(),line.end(),L',',L'|');auto fields=internalSplit(line.c_str());return fields.size()>6?_wtoi(fields[6].c_str())!=0:GetPrivateProfileIntW(L"desktop",L"clock-shadow",0,ini.c_str())!=0;
        }
        wchar_t text[128]={};auto fallback=std::to_wstring(d->fallback);GetPrivateProfileStringW(d->section,d->key,fallback.c_str(),text,128,ini.c_str());auto value=wcstod(text,nullptr);return std::isfinite(value)?value:d->fallback;
    }
    bool internalEnabled(SettingId id){
        using I=SettingId;auto d=island_settings::find(id);bool desktopColor=!wcscmp(d->section,L"desktop-colors")||id==I::DesktopThemeDark;
        if((desktopColor||(id>=I::DesktopMonitor&&id<=I::DesktopShade))&&!desktopEnabled)return false;
        if((id==I::PanelVideo||id==I::DesktopVideo||id==I::DesktopEngine||id==I::DesktopEngineCurrent||id==I::VideoFPS)&&!performance.video)return false;
        if((id==I::Outline||id==I::OutlineSmooth)&&BorderWidth<=0)return false;
        if((id==I::DesktopOutline||id==I::DesktopOutlineSmooth)&&desktopLook.width<=0)return false;
        return true;
    }
    void internalApply(SettingId id,double value=0,bool action=false){
        auto definition=island_settings::find(id);
        if(action&&definition&&*definition->key&&(!wcscmp(definition->section,L"colors")||!wcscmp(definition->section,L"desktop-colors"))){
            const wchar_t* keys[]={L"background",L"card",L"accent",L"text",L"secondary",L"hover",L"outline"};for(int i=0;i<7;i++)if(!wcscmp(keys[i],definition->key)){appearanceDesktop=!wcscmp(definition->section,L"desktop-colors");selectedColor=i;openColorPicker();return;}
        }
        island_settings::Packet p;p.id=id;p.value=value;p.action=action;p.owner=(uint64_t)(UINT_PTR)hwnd;
        if(!nativeSetting(p))showToast(L"Не удалось изменить настройку");draw();
    }
    void internalClockPicker(){
        int selected=(int)internalValue(SettingId::ClockStyle);
        for(int i=0;i<9;i++){
            Box b{34+(i%3)*226.f,96+(i/3)*61.f,216,55};button(b,L"",nullptr,[this,i]{internalApply(SettingId::ClockStyle,i+1,true);internalClock=false;draw();});
            if(i==selected)gfx->stroke(b,Accent,8,1.5f);float cx=b.x+b.w/2,cy=b.y+b.h/2;
            if(i==2||i==3||i==8){gfx->stroke({cx-21,cy-21,42,42},Ink,21,1.5f);if(i==2){gfx->line(cx,cy,cx-12,cy-7,Ink,2);gfx->line(cx,cy,cx+3,cy-17,Ink,2);}else if(i==3)gfx->text(L"15:42",{cx-21,cy-10,42,20},11,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);else{gfx->fill({cx+17,cy-4,8,8},Accent,4);gfx->line(cx,cy,cx-9,cy+9,Ink,2);}}
            else if(i==5){gfx->text(L"15",{cx-25,cy-24,50,25},19,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);gfx->text(L"42",{cx-25,cy,50,25},19,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);}
            else{gfx->text(i==4?L"15 / 42":i==1?L"15 42":L"15:42",b,24,i==7?Accent:Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);if(i==1)gfx->line(cx-42,cy,cx+42,cy,Background,2);if(i==6)gfx->stroke({cx-45,cy-18,90,36},Accent,4,1);}
        }
        button({34,287,110,27},L"Назад",L"\uE72B",[this]{internalClock=false;draw();});
    }
    bool internalColor(SettingId id,unsigned& color)const{
        auto d=island_settings::find(id);if(!d||d->kind!=island_settings::Kind::Action||(wcscmp(d->section,L"colors")&&wcscmp(d->section,L"desktop-colors")))return false;
        const wchar_t* keys[]={L"background",L"card",L"accent",L"text",L"secondary",L"hover",L"outline"};
        const unsigned panel[]={Background,Card,Accent,Ink,Muted,Hover,Outline};
        for(int i=0;i<7;i++)if(!wcscmp(d->key,keys[i])){color=!wcscmp(d->section,L"desktop-colors")?desktopLook.colors[i]:panel[i];return true;}return false;
    }
    std::wstring internalLabel(SettingId id){std::wstring label=island_settings::find(id)->label;unsigned color;if(internalColor(id,color))label+=L"  "+hexColor(color);return label;}
    float internalButtonWidth(SettingId id){unsigned color;return std::clamp(gfx->textWidth(internalLabel(id),12)+(internalColor(id,color)?52:28),100.f,334.f);}
    std::vector<std::vector<SettingId>> internalLayout(){
        std::vector<std::vector<SettingId>> layout;float used=0;bool actions=false;
        for(auto id:internalRows()){bool action=island_settings::find(id)->kind==island_settings::Kind::Action;float width=action?internalButtonWidth(id):668;
            if(!action||!actions||used+width>668){layout.emplace_back();used=0;}layout.back().push_back(id);used+=width+8;actions=action;
        }return layout;
    }
    void internalFooter(){gfx->text(L"By Shizuka | Tg @idontluvyou",{34,307,668,20},10,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);}
    void internalSliderPointer(float x){auto d=island_settings::find(internalSlideId);internalSlideValue=std::clamp(d->minimum+std::round(std::clamp((x-434)/256,0.f,1.f)*(d->maximum-d->minimum)/d->step)*d->step,d->minimum,d->maximum);draw();}
    void internalFinishSlider(){if(!internalDragging)return;internalDragging=false;auto id=internalSlideId;auto value=internalSlideValue;ReleaseCapture();internalApply(id,value);}
    void internalSettingsPage(){
        using I=SettingId;using K=island_settings::Kind;
        const wchar_t* sections[]={L"Основное",L"Оформление",L"Рабочий стол",L"Производительность"};float widths[]={128,153,166,197};float x=34;
        for(int i=0;i<4;i++){Box b{x,54,widths[i],30};button(b,sections[i],nullptr,[this,i]{if(internalDragging)internalFinishSlider();internalSection=i;internalOffset=0;internalClock=false;draw();});if(i==internalSection)gfx->stroke(b,Accent,8,1);x+=widths[i]+8;}
        internalFooter();if(internalClock){internalClockPicker();return;}
        auto layout=internalLayout();int maximum=std::max(0,(int)layout.size()-InternalVisibleRows);internalOffset=std::clamp(internalOffset,0,maximum);SetPropW(hwnd,L"OwnIsland.SettingsSection",(HANDLE)(UINT_PTR)(internalSection+1));SetPropW(hwnd,L"OwnIsland.SettingsOffset",(HANDLE)(UINT_PTR)(internalOffset+1));
        for(int slot=0;slot<InternalVisibleRows&&slot+internalOffset<(int)layout.size();slot++){
            float y=96+slot*32.f,x=34;
            for(auto id:layout[slot+internalOffset]){
                auto d=island_settings::find(id);bool enabled=internalEnabled(id);double value=internalDragging&&internalSlideId==id?internalSlideValue:internalValue(id);Box row{34,y,668,27};
                if(d->kind==K::Action){
                    float width=internalButtonWidth(id);unsigned color;bool swatch=internalColor(id,color);
                    button({x,y,width,27},swatch?L"":internalLabel(id),nullptr,[this,id]{if(id==I::ClockStyle){internalClock=true;draw();}else internalApply(id,0,true);},false,enabled);
                    if(swatch){Box circle{x+10,y+6.5f,14,14};gfx->fill(circle,color,7,1,false);gfx->stroke(circle,mix(color,Ink,.45f),7,1);gfx->text(internalLabel(id),{x+33,y,width-43,27},12,enabled?Ink:mix(Card,Muted,.7f),false,DWRITE_TEXT_ALIGNMENT_LEADING,true);}
                    x+=width+8;
                }
                else{
                    gfx->text(d->label,{47,y,345,27},12,enabled?Ink:Muted);
                    if(d->kind==K::Boolean){gfx->fill({646,y+3,39,21},value?Accent:Hover,11);gfx->fill({value?666.f:649.f,y+6,15,15},enabled?Ink:Muted,8);if(enabled)addHit(row,[this,id,value](float){internalApply(id,!value);});}
                    else if(d->kind==K::Range){
                        if(id==I::PanelShade||id==I::DesktopShade||id==I::PanelOpacity||id==I::PanelBlur){
                            gfx->text(decimal(value)+(id==I::PanelBlur?L" px":L" %"),{352,y,65,27},11,enabled?Ink:Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);float amount=(float)((value-d->minimum)/(d->maximum-d->minimum));gfx->fill({434,y+12,256,3},Hover,1.5f);gfx->fill({434,y+12,256*amount,3},enabled?Accent:Muted,1.5f);gfx->fill({430+256*amount,y+9,8,9},enabled?Ink:Muted,4);if(enabled)addHit({430,y,264,27},[this,id](float f){internalSlideId=id;internalDragging=true;SetCapture(hwnd);internalSliderPointer(430+264*f);});
                        }else{
                            gfx->text(decimal(value),{581,y,55,27},12,enabled?Ink:Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);button({538,y,34,27},L"−",nullptr,[this,id,value,d]{internalApply(id,std::max(d->minimum,value-d->step));},false,enabled&&value>d->minimum);button({654,y,34,27},L"+",nullptr,[this,id,value,d]{internalApply(id,std::min(d->maximum,value+d->step));},false,enabled&&value<d->maximum);
                        }
                    }else{
                        auto labels=internalSplit(d->choices),strings=internalSplit(d->values);std::vector<double> values;for(auto& s:strings)values.push_back(wcstod(s.c_str(),nullptr));size_t selected=0;for(size_t i=0;i<values.size();i++)if(std::abs(values[i]-value)<.001)selected=i;
                        button({510,y,184,27},labels.empty()?L"":labels[selected],L"\uE70D",[this,id,labels,values,value]{std::vector<MenuItem> items;for(size_t i=0;i<labels.size();i++)items.push_back({labels[i],nullptr,(int)i+1,std::abs(values[i]-value)<.001});POINT point{};GetCursorPos(&point);dialog=true;auto selected=customMenu(hwnd,point,items,menuColors());dialog=false;if(selected>0&&selected<=(int)values.size())internalApply(id,values[selected-1]);},false,enabled);
                    }
                }
            }
        }
        if(maximum>0){float height=188.f,thumb=std::max(22.f,height*InternalVisibleRows/layout.size());gfx->fill({708,96,3,height},Hover,1.5f);gfx->fill({708,96+(height-thumb)*internalOffset/maximum,3,thumb},Muted,1.5f);}
    }
