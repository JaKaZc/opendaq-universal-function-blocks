// Function blocks that measure as engineers do, on whatever device runs them:
//
//   Vibration (UniversalVibration)                velocity RMS in mm/s as ISO 10816
//                                                    has it, from up to three axes
//   Sound level meter (UniversalSoundLevelMeter)  weighted sound levels, Leq,
//                                                    maximum and octave bands
#pragma once

#include "signal_reader.h"

#include <opendaq/function_block_impl.h>
#include <opendaq/opendaq.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace daq::modules::universal_renderer
{

/// A second-order section, y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    double step(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }

    static Biquad highPass(double cutoff, double rate);
};

class VibrationFb final : public FunctionBlock
{
public:
    VibrationFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId);

    static FunctionBlockTypePtr CreateType(const ModuleInfoPtr& moduleInfo);

    void onPacketReceived(const InputPortPtr& port) override;

private:
    /// One axis: its filters, the sums of the current window, and the result
    /// of the last one.
    struct Axis
    {
        InputPortConfigPtr port;
        SignalReader reader;
        double rate = 0;
        Biquad accelerationFilter;
        Biquad velocityFilter;
        double velocity = 0;
        double previous = 0;
        bool started = false;

        int64_t window = INT64_MIN;
        double sumAcceleration = 0;
        double sumVelocity = 0;
        double peak = 0;
        size_t count = 0;
        std::vector<double> block;

        bool hasResult = false;
        int64_t resultWindow = INT64_MIN;
        double accelerationRms = 0;
        double velocityRms = 0;
        double resultPeak = 0;
        double dominant = 0;
    };

    void reset(Axis& axis);
    void finish(Axis& axis, size_t index);
    void describe();

    std::array<Axis, 3> axes;
    std::mutex lock;
    SignalConfigPtr time;
    SignalConfigPtr velocityRms;
    SignalConfigPtr accelerationRms;
    SignalConfigPtr peak;
    SignalConfigPtr crestFactor;
    SignalConfigPtr dominantFrequency;
    SignalConfigPtr zone;

    std::atomic<double> window{1.0};
    std::atomic<double> highPass{10.0};
    std::atomic<int> machineClass{0};
    std::atomic<int> version{0};
    int describedVersion = -1;
    int64_t windowMicroseconds = 1'000'000;
    std::string origin;
};

class SoundLevelMeterFb final : public FunctionBlock
{
public:
    SoundLevelMeterFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId);

    static FunctionBlockTypePtr CreateType(const ModuleInfoPtr& moduleInfo);

    void onPacketReceived(const InputPortPtr& port) override;

private:
    void configure();
    void emitLevels(int64_t end);
    void emitBands(double endSeconds);

    InputPortConfigPtr input;
    SignalReader reader;
    SignalConfigPtr time;
    SignalConfigPtr level;
    SignalConfigPtr leq;
    SignalConfigPtr maximum;
    SignalConfigPtr bandsTime;
    SignalConfigPtr bands;

    std::atomic<int> frequencyWeighting{0};  // A, C, Z
    std::atomic<int> timeWeighting{0};       // Fast, Slow
    std::atomic<int> bandKind{1};            // none, octaves, third octaves
    std::atomic<double> referenceSetting{0};
    std::atomic<int> version{0};
    std::atomic<bool> resetRequested{false};
    int configuredVersion = -1;
    double configuredRate = 0;

    std::vector<Biquad> weighting;
    double weightingGain = 1;
    double meanSquare = 0;
    double alpha = 0;
    double reference = 1;
    double energy = 0;
    double energyCount = 0;
    double maxLevel = -1e300;
    int64_t outputIndex = INT64_MIN;
    std::vector<double> centres;
    std::deque<double> raw;
    size_t bandBlock = 0;
};

}  // namespace daq::modules::universal_renderer
