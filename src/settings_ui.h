#pragma once
#include <windows.h>
#include <string>
bool openSystemSettings(const wchar_t* page=L"Island_SettingsPage");
bool settingsLightTheme();
bool chooseSettingsColor(HWND owner,unsigned initial,unsigned background,unsigned text,unsigned accent,unsigned& result);
int chooseSettingsClock(HWND owner,int current,unsigned background,unsigned text,unsigned accent);
