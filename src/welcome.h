#pragma once
#include <windows.h>
#include <memory>

class WelcomeAnimation {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    WelcomeAnimation();
    ~WelcomeAnimation();
    void start(HWND owner, bool preview=false);
    void update(bool desktopReady, bool awake);
    void displayChanged();
    void stop();
    bool active()const;
};
