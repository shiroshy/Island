// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
enum class NetworkKind {Disconnected,Wifi,Ethernet,Other};
inline const wchar_t* networkGlyph(NetworkKind kind){return kind==NetworkKind::Wifi?L"\uE701":kind==NetworkKind::Ethernet?L"\uE839":kind==NetworkKind::Other?L"\uE968":L"\uEB55";}
class NetworkMonitor {
    std::atomic<NetworkKind> current=NetworkKind::Disconnected;
    std::mutex mutex;std::condition_variable wake;std::thread worker;bool stopping=false;
public:
    void start();void stop();~NetworkMonitor(){stop();}
    NetworkKind state()const{return current.load();}
};
