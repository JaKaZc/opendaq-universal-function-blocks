#include "derived.h"

#include <opendaq/component_type_private.h>

#include <cmath>
#include <complex>
#include <cstring>

namespace daq::modules::universal_renderer
{

namespace
{

/// The lowest level reported, for silence: below any real measurement.
constexpr double kFloorDb = -160.0;

constexpr double kPi = 3.14159265358979323846;
const int kBlockSizes[] = {256, 512, 1024, 2048, 4096, 8192};

FunctionBlockTypePtr typeWith(const char* id, const char* name, const char* description, const ModuleInfoPtr& moduleInfo)
{
    auto type = FunctionBlockType(id, name, description);
    if (moduleInfo.assigned())
        checkErrorInfo(type.asPtr<IComponentTypePrivate>(true)->setModuleInfo(moduleInfo));
    return type;
}

/// The output's time: ticks of a microsecond from the input's origin.
DataDescriptorPtr timeDescriptor(const std::string& origin, int64_t delta)
{
    auto builder = DataDescriptorBuilder()
                       .setSampleType(SampleType::Int64)
                       .setTickResolution(Ratio(1, 1'000'000))
                       .setRule(LinearDataRule(std::max<int64_t>(1, delta), 0))
                       .setUnit(Unit("s", -1, "seconds", "time"))
                       .setName("Time");
    if (!origin.empty())
        builder.setOrigin(origin);
    return builder.build();
}

/// An in-place radix-2 FFT.
void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1)
    {
        const double angle = -2 * kPi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += length)
        {
            std::complex<double> w(1);
            for (size_t k = 0; k < length / 2; k++)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + length / 2] * w;
                a[i + k] = u + v;
                a[i + k + length / 2] = u - v;
                w *= step;
            }
        }
    }
}

int64_t floorDivide(int64_t a, int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

}  // namespace

// ---------------------------------------------------------------- LevelFb

FunctionBlockTypePtr LevelFb::CreateType(const ModuleInfoPtr& moduleInfo)
{
    return typeWith("UniversalLevel",
                    "Level",
                    "RMS, peak and level in dB of a signal over a window, such as a sound level or a vibration level",
                    moduleInfo);
}

LevelFb::LevelFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId)
    : FunctionBlock(CreateType(moduleInfo), context, parent, localId)
{
    initComponentStatus();
    objPtr.addProperty(FloatPropertyBuilder("Window", 0.1)
                           .setUnit(Unit("s"))
                           .setMinValue(0.01)
                           .setMaxValue(60.0)
                           .setDescription("The time each value covers; one value per window.")
                           .build());
    objPtr.addProperty(FloatPropertyBuilder("Reference", 1.0)
                           .setDescription("The RMS value that is 0 dB. With 1, the level of a microphone signal from -1 to 1 is in dB of "
                                           "full scale.")
                           .build());
    objPtr.getOnPropertyValueWrite("Window") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        window = static_cast<double>(args.getValue());
        version++;
    };
    objPtr.getOnPropertyValueWrite("Reference") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        reference = static_cast<double>(args.getValue());
    };

    input = createAndAddInputPort("Input", PacketReadyNotification::SameThread);
    time = createAndAddSignal("Time", nullptr, false);
    rms = createAndAddSignal("Rms", nullptr);
    rms.setDescription("The root mean square of the input over the window.");
    peak = createAndAddSignal("Peak", nullptr);
    peak.setDescription("The largest absolute value of the input in the window.");
    level = createAndAddSignal("Level", nullptr);
    level.setDescription("The RMS in decibels relative to the Reference property.");
    for (const auto& signal : {rms, peak, level})
        signal.setDomainSignal(time);
    setComponentStatusWithMessage(ComponentStatus::Warning, "Connect a signal to the input.");
}

void LevelFb::describe()
{
    describedVersion = version;
    windowMicroseconds = std::max<int64_t>(1000, std::llround(window.load() * 1e6));
    time.setDescriptor(timeDescriptor(reader.origin, windowMicroseconds));
    rms.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(Unit(reader.unit)).setName("Rms").build());
    peak.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(Unit(reader.unit)).setName("Peak").build());
    level.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(Unit("dB", -1, "decibels", "level")).setName("Level").build());
    windowIndex = INT64_MIN;
    count = 0;
    sumSquares = peakValue = 0;
}

void LevelFb::emit(int64_t end)
{
    const double root = std::sqrt(sumSquares / static_cast<double>(count));
    const double ratio = root / std::max(1e-300, std::abs(reference.load()));
    const double values[3] = {root, peakValue, ratio > 0 ? std::max(kFloorDb, 20 * std::log10(ratio)) : kFloorDb};
    const auto domainPacket = DataPacket(time.getDescriptor(), 1, end);
    time.sendPacket(domainPacket);
    const SignalConfigPtr outputs[3] = {rms, peak, level};
    for (int i = 0; i < 3; i++)
    {
        const auto packet = DataPacketWithDomain(domainPacket, outputs[i].getDescriptor(), 1);
        *static_cast<double*>(packet.getRawData()) = values[i];
        outputs[i].sendPacket(packet);
    }
}

void LevelFb::onPacketReceived(const InputPortPtr& /*port*/)
{
    const auto connection = input.getConnection();
    if (!connection.assigned())
        return;
    std::vector<double> times;
    std::vector<double> values;
    for (PacketPtr packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (reader.event(packet))
        {
            if (reader.numbers)
            {
                describe();
                setComponentStatus(ComponentStatus::Ok);
            }
            else
            {
                setComponentStatusWithMessage(ComponentStatus::Warning, "Level reads signals with one number per sample.");
            }
            continue;
        }
        if (packet.getType() != PacketType::Data || !reader.numbers)
            continue;
        if (describedVersion != version)
            describe();
        reader.read(packet.asPtr<IDataPacket>(true), times, values);
        for (size_t i = 0; i < times.size(); i++)
        {
            const auto at = static_cast<int64_t>(std::llround(times[i] * 1e6));
            const int64_t index = floorDivide(at, windowMicroseconds);
            if (index != windowIndex)
            {
                if (count > 0 && windowIndex != INT64_MIN)
                    emit((windowIndex + 1) * windowMicroseconds);
                windowIndex = index;
                count = 0;
                sumSquares = peakValue = 0;
            }
            sumSquares += values[i] * values[i];
            peakValue = std::max(peakValue, std::abs(values[i]));
            count++;
        }
    }
}

// ---------------------------------------------------------------- SpectrumFb

FunctionBlockTypePtr SpectrumFb::CreateType(const ModuleInfoPtr& moduleInfo)
{
    return typeWith("UniversalSpectrum",
                    "Spectrum",
                    "The amplitude spectrum of a signal with a fixed sample rate, by FFT, as an array per block",
                    moduleInfo);
}

SpectrumFb::SpectrumFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId)
    : FunctionBlock(CreateType(moduleInfo), context, parent, localId)
{
    initComponentStatus();
    auto sizes = List<IString>();
    for (const int size : kBlockSizes)
        sizes.pushBack(std::to_string(size));
    objPtr.addProperty(SelectionPropertyBuilder("BlockSize", sizes, 3)
                           .setDescription("The samples per FFT. More gives finer frequencies and fewer spectra a second.")
                           .build());
    objPtr.addProperty(SelectionPropertyBuilder("Scale", List<IString>("dB", "Linear"), 0)
                           .setDescription("Amplitudes in decibels relative to the Reference property, or in the input's unit.")
                           .build());
    objPtr.addProperty(FloatPropertyBuilder("Reference", 1.0).setDescription("The amplitude that is 0 dB.").build());
    objPtr.getOnPropertyValueWrite("BlockSize") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        blockSize = kBlockSizes[std::clamp<Int>(args.getValue(), 0, 5)];
        version++;
    };
    objPtr.getOnPropertyValueWrite("Scale") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        decibels = static_cast<Int>(args.getValue()) == 0;
        version++;
    };
    objPtr.getOnPropertyValueWrite("Reference") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        reference = static_cast<double>(args.getValue());
    };

    input = createAndAddInputPort("Input", PacketReadyNotification::SameThread);
    time = createAndAddSignal("Time", nullptr, false);
    spectrum = createAndAddSignal("Spectrum", nullptr);
    spectrum.setDescription("The amplitude of each frequency, from 0 to half the sample rate.");
    spectrum.setDomainSignal(time);
    setComponentStatusWithMessage(ComponentStatus::Warning, "Connect a signal to the input.");
}

void SpectrumFb::describe()
{
    describedVersion = version;
    describedRate = reader.sampleRate;
    const int n = blockSize;
    const double resolution = reader.sampleRate / n;
    const auto dimension = DimensionBuilder()
                               .setRule(LinearDimensionRule(resolution, 0.0, static_cast<SizeT>(n / 2)))
                               .setUnit(Unit("Hz", -1, "hertz", "frequency"))
                               .setName("Frequency")
                               .build();
    spectrum.setDescriptor(DataDescriptorBuilder()
                               .setSampleType(SampleType::Float32)
                               .setDimensions(List<IDimension>(dimension))
                               .setUnit(decibels ? Unit("dB", -1, "decibels", "level") : Unit(reader.unit))
                               .setName("Spectrum")
                               .build());
    time.setDescriptor(timeDescriptor(reader.origin, std::llround(n / 2 / reader.sampleRate * 1e6)));
    samples.clear();
}

void SpectrumFb::emit(double endSeconds)
{
    const size_t n = static_cast<size_t>(blockSize.load());
    std::vector<std::complex<double>> buffer(n);
    double gain = 0;
    for (size_t i = 0; i < n; i++)
    {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        gain += w;
        buffer[i] = samples[i] * w;
    }
    fft(buffer);
    const auto domainPacket = DataPacket(time.getDescriptor(), 1, std::llround(endSeconds * 1e6));
    const auto packet = DataPacketWithDomain(domainPacket, spectrum.getDescriptor(), 1);
    auto* out = static_cast<float*>(packet.getRawData());
    const double ref = std::max(1e-300, std::abs(reference.load()));
    for (size_t k = 0; k < n / 2; k++)
    {
        const double amplitude = std::abs(buffer[k]) * (k == 0 ? 1.0 : 2.0) / gain;
        out[k] = static_cast<float>(decibels ? std::max(kFloorDb, 20 * std::log10(std::max(amplitude, 1e-300) / ref)) : amplitude);
    }
    time.sendPacket(domainPacket);
    spectrum.sendPacket(packet);
}

void SpectrumFb::onPacketReceived(const InputPortPtr& /*port*/)
{
    const auto connection = input.getConnection();
    if (!connection.assigned())
        return;
    std::vector<double> times;
    std::vector<double> values;
    for (PacketPtr packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (reader.event(packet))
        {
            if (reader.numbers && reader.sampleRate > 0)
            {
                describe();
                setComponentStatus(ComponentStatus::Ok);
            }
            else
            {
                setComponentStatusWithMessage(ComponentStatus::Warning,
                                              "Spectrum reads signals with one number per sample at a fixed sample rate.");
            }
            continue;
        }
        if (packet.getType() != PacketType::Data || !reader.numbers || reader.sampleRate <= 0)
            continue;
        if (describedVersion != version || describedRate != reader.sampleRate)
            describe();
        reader.read(packet.asPtr<IDataPacket>(true), times, values);
        if (times.empty())
            continue;
        samples.insert(samples.end(), values.begin(), values.end());
        lastTime = times.back();
        const size_t n = static_cast<size_t>(blockSize.load());
        while (samples.size() >= n)
        {
            // The block ends at its last sample, which lies this far before
            // the newest one.
            emit(lastTime - static_cast<double>(samples.size() - n) / reader.sampleRate);
            samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(n / 2));
        }
    }
}

}  // namespace daq::modules::universal_renderer
