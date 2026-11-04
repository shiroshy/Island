// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <algorithm>
#include <array>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <complex>

struct SpectrumFrame {
    std::array<float,48> bars{};
    std::array<float,128> wave{};
    float rms=0;
};
class SpectrumAnalyzer {
    std::array<float,4096> samples{};
    size_t cursor=0,total=0;
public:
    bool feed(float sample);
    SpectrumFrame calculate(unsigned sampleRate)const;
};

class AudioMeter {
    std::array<std::atomic<float>,3> bands{};
    std::atomic<bool> active=false,stopping=false;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> detailed=false;
    std::atomic<unsigned> generation=0;
    std::atomic<int> analysisRate=30;
    mutable std::mutex spectrumMutex;
    SpectrumFrame spectrumFrame;
    void run();
public:
    void start(){worker=std::thread([this]{run();});}
    void enable(bool on){if(active.exchange(on)!=on)wake.notify_all();}
    std::array<float,3> levels() const{return {bands[0].load(),bands[1].load(),bands[2].load()};}
    void restart(){++generation;wake.notify_all();}
    void fullSpectrum(bool enabled){detailed=enabled;}
    void setAnalysisRate(int fps){analysisRate=std::clamp(fps,5,60);}
    SpectrumFrame spectrum()const{std::lock_guard lock(spectrumMutex);return spectrumFrame;}
    // Desktop receives the panel's analysis; no loopback worker is started there.
    void importSpectrum(const SpectrumFrame& frame){std::lock_guard lock(spectrumMutex);spectrumFrame=frame;}
    ~AudioMeter(){stopping=true;wake.notify_all();if(worker.joinable())worker.join();}
};
