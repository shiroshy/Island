#pragma once
#include "support.h"
#include "package_manifest.h"
namespace setup {
inline bool ownedInstall(const std::wstring& root){return ini(root+L"\\installed.ini",L"product")==ProductTag||ini(root+L"\\transaction.ini",L"product")==ProductTag;}
// This distribution never opens Windows Settings resources or SettingId keys.
inline void preflightNative(const std::wstring&){}
inline void removeNative(const std::wstring&){}
struct NativeTransaction {explicit NativeTransaction(const std::wstring&){}void apply(){}void rollback(){}};
}
