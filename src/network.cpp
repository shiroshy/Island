// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include <winsock2.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#include "network.h"
#include <vector>
#include <chrono>
namespace {
NetworkKind connection(){
    ULONG size=16384;std::vector<unsigned char> bytes(size);ULONG error=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,(IP_ADAPTER_ADDRESSES*)bytes.data(),&size);if(error==ERROR_BUFFER_OVERFLOW&&size<4*1024*1024){bytes.resize(size);error=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,(IP_ADAPTER_ADDRESSES*)bytes.data(),&size);}if(error!=NO_ERROR)return NetworkKind::Disconnected;
    DWORD best=0;GetBestInterface(0x08080808,&best);NetworkKind selected=NetworkKind::Disconnected;ULONG score=0;
    for(auto a=(IP_ADAPTER_ADDRESSES*)bytes.data();a;a=a->Next){if(a->OperStatus!=IfOperStatusUp||!a->FirstUnicastAddress||a->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;bool physical=a->IfType==IF_TYPE_IEEE80211||a->IfType==IF_TYPE_ETHERNET_CSMACD;ULONG priority=(physical?20:1)+(a->FirstGatewayAddress?10:0)+(physical&&a->IfIndex==best?100:0);if(priority>score){score=priority;selected=a->IfType==IF_TYPE_IEEE80211?NetworkKind::Wifi:a->IfType==IF_TYPE_ETHERNET_CSMACD?NetworkKind::Ethernet:NetworkKind::Other;}}
    return selected;
}
}
void NetworkMonitor::start(){if(worker.joinable())return;stopping=false;worker=std::thread([this]{for(;;){current=connection();std::unique_lock lock(mutex);if(wake.wait_for(lock,std::chrono::seconds(2),[this]{return stopping;}))break;}});}
void NetworkMonitor::stop(){{std::lock_guard lock(mutex);stopping=true;}wake.notify_all();if(worker.joinable())worker.join();}
