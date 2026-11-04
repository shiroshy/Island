// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "menu.h"
#include <memory>
class TrayHost {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    TrayHost();~TrayHost();
    bool start(HWND owner);void stop();void maintain();
    void setAwake(bool awake);
    bool hosted()const;bool visible()const;bool contextInteraction()const;
    bool toggle(POINT anchor);void close();
    void colors(MenuColors colors);
    unsigned count()const;
};

bool isIslandTrayEndpoint(HWND window);
