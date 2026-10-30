// Function blocks that derive signals from one input signal, on whatever
// device runs them, such as a phone, so every client gets the result:
//
//   Level (UniversalLevel)        RMS, peak and level in dB over a window
//   Spectrum (UniversalSpectrum)  the amplitude spectrum, as an array per sample
#pragma once

#include "signal_reader.h"

#include <opendaq/function_block_impl.h>
#include <opendaq/opendaq.h>

#include <atomic>
#include <deque>
#include <vector>

namespace daq::modules::universal_renderer
{

/// RMS, peak and level in dB of the input over a window, such as the sound
/// level of a microphone or the vibration of an accelerometer.
class LevelFb final : public FunctionBlock
{
public:
    LevelFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId);

    static FunctionBlockTypePtr CreateType(const ModuleInfoPtr& moduleInfo);

    void onPacketReceived(const InputPortPtr& port) override;

private:
    void describe();
    void emit(int64_t end);

    InputPortConfigPtr input;
    SignalReader reader;
    SignalConfigPtr time;
    SignalConfigPtr rms;
    SignalConfigPtr peak;
    SignalConfigPtr level;

    std::atomic<double> window{0.1};
    std::atomic<double> reference{1.0};
    std::atomic<int> version{0};
    int describedVersion = -1;
    int64_t windowMicroseconds = 0;

    int64_t windowIndex = INT64_MIN;
    double sumSquares = 0;
    double peakValue = 0;
    size_t count = 0;
};

/// The amplitude spectrum of the input, by FFT over blocks that overlap by
/// half, with a Hann window: one array per block, its elements the
/// frequencies from 0 to half the sample rate.
class SpectrumFb final : public FunctionBlock
{
public:
    SpectrumFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId);

    static FunctionBlockTypePtr CreateType(const ModuleInfoPtr& moduleInfo);

    void onPacketReceived(const InputPortPtr& port) override;

private:
    void describe();
    void emit(double endSeconds);

    InputPortConfigPtr input;
    SignalReader reader;
    SignalConfigPtr time;
    SignalConfigPtr spectrum;

    std::atomic<int> blockSize{2048};
    std::atomic<bool> decibels{true};
    std::atomic<double> reference{1.0};
    std::atomic<int> version{0};
    int describedVersion = -1;
    double describedRate = 0;

    std::deque<double> samples;
    double lastTime = 0;
};

}  // namespace daq::modules::universal_renderer
