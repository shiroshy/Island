// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "graphics.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

struct MediaState {
    bool available = false, playing = false;
    std::wstring title, artist, source;
    double position = 0, duration = 0;
    std::shared_ptr<const std::vector<unsigned char>> cover;
    unsigned long long revision = 0;
};
class MediaBridge {
    std::mutex mutex;
    MediaState state;
    std::atomic<bool> stopping = false;
    std::atomic<int> command = 0;
    std::atomic<double> seekTo = -1;
    std::thread worker;
public:
    void start(HWND target);
    void stop();
    ~MediaBridge() { stop(); }
    MediaState snapshot();
    void control(int action) { command.store(action); }
    void seek(double seconds) { seekTo.store(seconds); }
};
std::wstring stateDirectory();
std::wstring readNote();
void writeNote(const std::wstring& value);
std::wstring clipboardText(HWND owner);
bool setClipboard(HWND owner, const std::wstring& value);
std::wstring chooseFile(HWND owner, const wchar_t* title);
std::wstring fileName(const std::wstring& path);
void openTarget(const std::wstring& target);
float getVolume();
bool getMuted();
int getMicState();
void setVolume(float volume);
void toggleMute();
void toggleMic();
void captureScreen(HWND island);
std::wstring transformText(const std::wstring& value, int action);
struct WeatherDay { std::wstring date; double low, high; int code; };
struct WeatherState {
    bool available=false;
    std::wstring city,error;
    long errorCode=0;
    double temperature=0,wind=0;
    int humidity=0,code=0;
    std::vector<WeatherDay> days;
};
WeatherState fetchWeather(const std::wstring& city);
