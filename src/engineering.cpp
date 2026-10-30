#include "engineering.h"

#include <coreobjects/callable_info_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <opendaq/component_type_private.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace daq::modules::universal_renderer
{

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kFloorDb = -160.0;

FunctionBlockTypePtr typeWith(const char* id, const char* name, const char* description, const ModuleInfoPtr& moduleInfo)
{
    auto type = FunctionBlockType(id, name, description);
    if (moduleInfo.assigned())
        checkErrorInfo(type.asPtr<IComponentTypePrivate>(true)->setModuleInfo(moduleInfo));
    return type;
}

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

DataDescriptorPtr valueDescriptor(const char* name, const UnitPtr& unit)
{
    auto builder = DataDescriptorBuilder().setSampleType(SampleType::Float64).setName(name);
    if (unit.assigned())
        builder.setUnit(unit);
    return builder.build();
}

int64_t floorDivide(int64_t a, int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

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

size_t powerOfTwoBelow(size_t n)
{
    size_t p = 1;
    while (p * 2 <= n)
        p *= 2;
    return p;
}

void addProcedure(const PropertyObjectPtr& object, const char* name, const char* description, std::function<void()> action)
{
    object.addProperty(FunctionPropertyBuilder(name, ProcedureInfo()).setReadOnly(true).setDescription(description).build());
    object.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue(name, Procedure([action] { action(); }));
}

/// The boundaries of ISO 10816-1's zones A/B, B/C and C/D in mm/s RMS, for
/// its machine classes I to IV.
const double kZoneLimits[4][3] = {{0.71, 1.8, 4.5}, {1.12, 2.8, 7.1}, {1.8, 4.5, 11.2}, {2.8, 7.1, 18.0}};

}  // namespace

Biquad Biquad::highPass(double cutoff, double rate)
{
    // Butterworth, by the bilinear transform.
    Biquad out;
    const double k = std::tan(kPi * std::min(cutoff, rate * 0.45) / rate);
    const double norm = 1 / (1 + std::sqrt(2.0) * k + k * k);
    out.b0 = norm;
    out.b1 = -2 * norm;
    out.b2 = norm;
    out.a1 = 2 * (k * k - 1) * norm;
    out.a2 = (1 - std::sqrt(2.0) * k + k * k) * norm;
    return out;
}

// ---------------------------------------------------------------- VibrationFb

FunctionBlockTypePtr VibrationFb::CreateType(const ModuleInfoPtr& moduleInfo)
{
    return typeWith("UniversalVibration",
                    "Vibration",
                    "Vibration as ISO 10816 measures it: velocity RMS in mm/s from up to three axes of acceleration in m/s², with the "
                    "acceleration RMS, peak, crest factor, dominant frequency and the zone for a machine class",
                    moduleInfo);
}

VibrationFb::VibrationFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId)
    : FunctionBlock(CreateType(moduleInfo), context, parent, localId)
{
    initComponentStatus();
    objPtr.addProperty(FloatPropertyBuilder("Window", 1.0)
                           .setUnit(Unit("s"))
                           .setMinValue(0.25)
                           .setMaxValue(60.0)
                           .setDescription("The time each value covers.")
                           .build());
    objPtr.addProperty(FloatPropertyBuilder("HighPass", 10.0)
                           .setUnit(Unit("Hz"))
                           .setMinValue(0.5)
                           .setMaxValue(200.0)
                           .setDescription("The lower edge of the band; ISO 10816 uses 10 Hz. The upper edge is half the input's sample rate, "
                                           "100 Hz for a phone's accelerometer at 200 Hz.")
                           .build());
    objPtr.addProperty(SelectionPropertyBuilder("MachineClass",
                                                List<IString>("I: small machines, up to 15 kW",
                                                              "II: medium machines, 15 to 75 kW",
                                                              "III: large machines on rigid foundations",
                                                              "IV: large machines on soft foundations"),
                                                0)
                           .setDescription("The class of ISO 10816-1 whose limits decide the Zone.")
                           .build());
    objPtr.getOnPropertyValueWrite("Window") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        window = static_cast<double>(args.getValue());
        version++;
    };
    objPtr.getOnPropertyValueWrite("HighPass") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        highPass = static_cast<double>(args.getValue());
        version++;
    };
    objPtr.getOnPropertyValueWrite("MachineClass") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        machineClass = static_cast<int>(static_cast<Int>(args.getValue()));
    };

    const char* const names[3] = {"X", "Y", "Z"};
    for (size_t i = 0; i < 3; i++)
        axes[i].port = createAndAddInputPort(names[i], PacketReadyNotification::SameThread);

    time = createAndAddSignal("Time", nullptr, false);
    velocityRms = createAndAddSignal("VelocityRms", nullptr);
    velocityRms.setDescription("The RMS of the vibration velocity in the band, the largest of the axes, as ISO 10816 judges it.");
    accelerationRms = createAndAddSignal("AccelerationRms", nullptr);
    accelerationRms.setDescription("The RMS of the acceleration in the band, over all axes.");
    peak = createAndAddSignal("Peak", nullptr);
    peak.setDescription("The largest acceleration in the band, of any axis.");
    crestFactor = createAndAddSignal("CrestFactor", nullptr);
    crestFactor.setDescription("Peak over RMS: about 1.4 for an even vibration, higher for knocks and impacts.");
    dominantFrequency = createAndAddSignal("DominantFrequency", nullptr);
    dominantFrequency.setDescription("The frequency with the most acceleration, in the axis with the most velocity.");
    zone = createAndAddSignal("Zone", nullptr);
    zone.setDescription("ISO 10816 zone for the machine class: 1 A (new machine), 2 B (fine for long operation), 3 C (fine for a "
                        "limited time), 4 D (damage likely).");
    for (const auto& signal : {velocityRms, accelerationRms, peak, crestFactor, dominantFrequency, zone})
        signal.setDomainSignal(time);
    setComponentStatusWithMessage(ComponentStatus::Warning, "Connect an acceleration signal in m/s² to X.");
}

void VibrationFb::describe()
{
    describedVersion = version;
    windowMicroseconds = std::max<int64_t>(250'000, std::llround(window.load() * 1e6));
    time.setDescriptor(timeDescriptor(origin, windowMicroseconds));
    velocityRms.setDescriptor(valueDescriptor("VelocityRms", Unit("mm/s", -1, "millimeters per second", "velocity")));
    accelerationRms.setDescriptor(valueDescriptor("AccelerationRms", Unit("m/s²", -1, "meters per second squared", "acceleration")));
    peak.setDescriptor(valueDescriptor("Peak", Unit("m/s²", -1, "meters per second squared", "acceleration")));
    crestFactor.setDescriptor(valueDescriptor("CrestFactor", nullptr));
    dominantFrequency.setDescriptor(valueDescriptor("DominantFrequency", Unit("Hz", -1, "hertz", "frequency")));
    zone.setDescriptor(valueDescriptor("Zone", nullptr));
    for (auto& axis : axes)
        reset(axis);
}

void VibrationFb::reset(Axis& axis)
{
    axis.rate = axis.reader.sampleRate;
    if (axis.rate > 0)
    {
        axis.accelerationFilter = Biquad::highPass(highPass, axis.rate);
        axis.velocityFilter = Biquad::highPass(highPass, axis.rate);
    }
    axis.velocity = 0;
    axis.previous = 0;
    axis.started = false;
    axis.window = INT64_MIN;
    axis.sumAcceleration = axis.sumVelocity = axis.peak = 0;
    axis.count = 0;
    axis.block.clear();
    axis.hasResult = false;
}

void VibrationFb::finish(Axis& axis, size_t index)
{
    axis.accelerationRms = std::sqrt(axis.sumAcceleration / static_cast<double>(axis.count));
    axis.velocityRms = std::sqrt(axis.sumVelocity / static_cast<double>(axis.count)) * 1000.0;
    axis.resultPeak = axis.peak;
    axis.dominant = 0;
    const size_t n = powerOfTwoBelow(axis.block.size());
    if (n >= 64)
    {
        std::vector<std::complex<double>> buffer(n);
        const size_t start = axis.block.size() - n;
        for (size_t i = 0; i < n; i++)
            buffer[i] = axis.block[start + i] * (0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n - 1)));
        fft(buffer);
        double best = 0;
        for (size_t k = 1; k < n / 2; k++)
        {
            const double frequency = static_cast<double>(k) * axis.rate / static_cast<double>(n);
            if (frequency < highPass * 0.7)
                continue;
            if (std::abs(buffer[k]) > best)
            {
                best = std::abs(buffer[k]);
                axis.dominant = frequency;
            }
        }
    }
    axis.hasResult = true;
    axis.resultWindow = axis.window;

    // The first connected axis sends, with the latest results of the others.
    size_t leader = 3;
    for (size_t i = 0; i < 3; i++)
        if (axes[i].port.getSignal().assigned())
        {
            leader = i;
            break;
        }
    if (index != leader)
        return;
    double velocityMax = 0;
    double accelerationSquares = 0;
    double peakMax = 0;
    double accelerationMax = 0;
    double dominant = 0;
    for (const auto& other : axes)
    {
        if (!other.hasResult || other.resultWindow < axis.resultWindow - 1)
            continue;
        accelerationSquares += other.accelerationRms * other.accelerationRms;
        peakMax = std::max(peakMax, other.resultPeak);
        accelerationMax = std::max(accelerationMax, other.accelerationRms);
        if (other.velocityRms >= velocityMax)
        {
            velocityMax = other.velocityRms;
            dominant = other.dominant;
        }
    }
    const auto& limits = kZoneLimits[std::clamp(machineClass.load(), 0, 3)];
    const double zoneValue = velocityMax < limits[0] ? 1 : velocityMax < limits[1] ? 2 : velocityMax < limits[2] ? 3 : 4;
    const double values[6] = {velocityMax, std::sqrt(accelerationSquares), peakMax, accelerationMax > 0 ? peakMax / accelerationMax : 0,
                              dominant, zoneValue};
    const auto domainPacket = DataPacket(time.getDescriptor(), 1, (axis.resultWindow + 1) * windowMicroseconds);
    time.sendPacket(domainPacket);
    const SignalConfigPtr outputs[6] = {velocityRms, accelerationRms, peak, crestFactor, dominantFrequency, zone};
    for (int i = 0; i < 6; i++)
    {
        const auto packet = DataPacketWithDomain(domainPacket, outputs[i].getDescriptor(), 1);
        *static_cast<double*>(packet.getRawData()) = values[i];
        outputs[i].sendPacket(packet);
    }
}

void VibrationFb::onPacketReceived(const InputPortPtr& port)
{
    std::lock_guard guard(lock);
    size_t index = 0;
    while (index < 3 && axes[index].port.getGlobalId() != port.getGlobalId())
        index++;
    if (index == 3)
        return;
    Axis& axis = axes[index];
    const auto connection = axis.port.getConnection();
    if (!connection.assigned())
        return;
    std::vector<double> times;
    std::vector<double> values;
    for (PacketPtr packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (axis.reader.event(packet))
        {
            if (axis.reader.numbers && axis.reader.sampleRate > 0)
            {
                if (index == 0 || origin.empty())
                    origin = axis.reader.origin;
                describedVersion = -1;
                setComponentStatus(ComponentStatus::Ok);
            }
            else
            {
                setComponentStatusWithMessage(ComponentStatus::Warning, "Vibration reads acceleration signals with a fixed sample rate.");
            }
            continue;
        }
        if (packet.getType() != PacketType::Data || !axis.reader.numbers || axis.reader.sampleRate <= 0)
            continue;
        if (describedVersion != version)
            describe();
        if (axis.rate != axis.reader.sampleRate)
            reset(axis);
        axis.reader.read(packet.asPtr<IDataPacket>(true), times, values);
        for (size_t i = 0; i < times.size(); i++)
        {
            const int64_t at = std::llround(times[i] * 1e6);
            const int64_t windowIndex = floorDivide(at, windowMicroseconds);
            if (windowIndex != axis.window)
            {
                if (axis.count > 0 && axis.window != INT64_MIN)
                    finish(axis, index);
                axis.window = windowIndex;
                axis.sumAcceleration = axis.sumVelocity = axis.peak = 0;
                axis.count = 0;
                axis.block.clear();
            }
            // Acceleration in the band, then integrated to velocity, whose
            // own drift the second filter takes off.
            const double acceleration = axis.accelerationFilter.step(values[i]);
            if (!axis.started)
            {
                axis.previous = acceleration;
                axis.started = true;
            }
            axis.velocity += (axis.previous + acceleration) * 0.5 / axis.rate;
            axis.previous = acceleration;
            const double velocity = axis.velocityFilter.step(axis.velocity);
            axis.sumAcceleration += acceleration * acceleration;
            axis.sumVelocity += velocity * velocity;
            axis.peak = std::max(axis.peak, std::abs(acceleration));
            axis.count++;
            if (axis.block.size() < 65536)
                axis.block.push_back(acceleration);
        }
    }
}

// ---------------------------------------------------------------- SoundLevelMeterFb

FunctionBlockTypePtr SoundLevelMeterFb::CreateType(const ModuleInfoPtr& moduleInfo)
{
    return typeWith("UniversalSoundLevelMeter",
                    "Sound level meter",
                    "Sound levels as a sound level meter measures them: A, C or Z weighted, Fast or Slow, with Leq, the maximum and "
                    "octave or third-octave bands",
                    moduleInfo);
}

SoundLevelMeterFb::SoundLevelMeterFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId)
    : FunctionBlock(CreateType(moduleInfo), context, parent, localId)
{
    initComponentStatus();
    objPtr.addProperty(SelectionPropertyBuilder("FrequencyWeighting", List<IString>("A", "C", "Z"), 0)
                           .setDescription("A follows the ear at moderate levels, C at loud ones; Z is unweighted.")
                           .build());
    objPtr.addProperty(SelectionPropertyBuilder("TimeWeighting", List<IString>("Fast", "Slow"), 0)
                           .setDescription("Fast averages over 0.125 s, Slow over 1 s.")
                           .build());
    objPtr.addProperty(SelectionPropertyBuilder("Bands", List<IString>("None", "Octaves", "Third octaves"), 1)
                           .setDescription("The unweighted levels of octave or third-octave bands, once a second.")
                           .build());
    objPtr.addProperty(FloatPropertyBuilder("Reference", 0.0)
                           .setDescription("The RMS that is 0 dB. With 0, it is 20 µPa for an input in pascals, which gives dB SPL, as a "
                                           "calibrated phone microphone sends, and 1 otherwise, which gives dB of full scale.")
                           .build());
    for (const char* name : {"FrequencyWeighting", "TimeWeighting", "Bands", "Reference"})
        objPtr.getOnPropertyValueWrite(name) += [this, name](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        {
            const std::string which = name;
            if (which == "FrequencyWeighting")
                frequencyWeighting = static_cast<int>(static_cast<Int>(args.getValue()));
            else if (which == "TimeWeighting")
                timeWeighting = static_cast<int>(static_cast<Int>(args.getValue()));
            else if (which == "Bands")
                bandKind = static_cast<int>(static_cast<Int>(args.getValue()));
            else
                referenceSetting = static_cast<double>(args.getValue());
            version++;
        };
    addProcedure(objPtr, "Reset", "Starts Leq and the maximum again.", [this] { resetRequested = true; });

    input = createAndAddInputPort("Input", PacketReadyNotification::SameThread);
    time = createAndAddSignal("Time", nullptr, false);
    level = createAndAddSignal("Level", nullptr);
    level.setDescription("The weighted, time-weighted sound level, ten times a second.");
    leq = createAndAddSignal("Leq", nullptr);
    leq.setDescription("The equivalent continuous level since the start or the last Reset.");
    maximum = createAndAddSignal("Max", nullptr);
    maximum.setDescription("The highest level since the start or the last Reset.");
    for (const auto& signal : {level, leq, maximum})
        signal.setDomainSignal(time);
    bandsTime = createAndAddSignal("BandsTime", nullptr, false);
    bands = createAndAddSignal("Bands", nullptr);
    bands.setDescription("The unweighted level of each octave or third-octave band, once a second.");
    bands.setDomainSignal(bandsTime);
    setComponentStatusWithMessage(ComponentStatus::Warning, "Connect a microphone signal to the input.");
}

void SoundLevelMeterFb::configure()
{
    configuredVersion = version;
    configuredRate = reader.sampleRate;
    const double fs = reader.sampleRate;

    // The weightings' poles (IEC 61672), each a first-order section by the
    // bilinear transform with prewarping, scaled to 0 dB at 1 kHz.
    weighting.clear();
    const auto section = [&](double frequency, bool high)
    {
        const double w = 2 * fs * std::tan(kPi * std::min(frequency, fs * 0.49) / fs);
        const double k = 2 * fs;
        Biquad b;
        b.b0 = high ? k / (k + w) : w / (k + w);
        b.b1 = high ? -k / (k + w) : w / (k + w);
        b.a1 = (w - k) / (k + w);
        weighting.push_back(b);
    };
    const int kind = frequencyWeighting;
    if (kind == 0 || kind == 1)
    {
        section(20.598997, true);
        section(20.598997, true);
        if (kind == 0)
        {
            section(107.65265, true);
            section(737.86223, true);
        }
        section(12194.217, false);
        section(12194.217, false);
    }
    // Scaled to the standard's response at a frequency the sampling
    // carries well: 1 kHz, or an eighth of the sample rate when that is
    // lower, where the standard's own formula gives the level.
    weightingGain = 1;
    if (!weighting.empty())
    {
        const double f = std::min(1000.0, fs / 8);
        const double f2 = f * f;
        const double p1 = 20.598997 * 20.598997;
        const double p4 = 12194.217 * 12194.217;
        const double analog = kind == 0 ? 20 * std::log10(p4 * f2 * f2 /
                                                          ((f2 + p1) * std::sqrt((f2 + 107.65265 * 107.65265) * (f2 + 737.86223 * 737.86223)) *
                                                           (f2 + p4))) + 2.00
                                        : 20 * std::log10(p4 * f2 / ((f2 + p1) * (f2 + p4))) + 0.06;
        const std::complex<double> z = std::polar(1.0, -2 * kPi * f / fs);
        std::complex<double> response(1);
        for (const auto& b : weighting)
            response *= (b.b0 + b.b1 * z) / (1.0 + b.a1 * z);
        weightingGain = std::pow(10.0, analog / 20) / std::abs(response);
    }
    alpha = 1 - std::exp(-1 / ((timeWeighting == 0 ? 0.125 : 1.0) * fs));
    reference = referenceSetting > 0 ? referenceSetting.load() : reader.unit == "Pa" ? 20e-6 : 1.0;
    meanSquare = 0;
    energy = energyCount = 0;
    maxLevel = -1e300;
    outputIndex = INT64_MIN;

    // Octave bands from 31.5 Hz, third octaves from 20 Hz, up to what the
    // sample rate carries.
    centres.clear();
    if (bandKind == 1)
        for (int k = -5; k <= 4; k++)
            centres.push_back(1000.0 * std::pow(2.0, k));
    else if (bandKind == 2)
        for (int k = -17; k <= 13; k++)
            centres.push_back(1000.0 * std::pow(2.0, k / 3.0));
    const double edge = bandKind == 1 ? std::sqrt(2.0) : std::pow(2.0, 1.0 / 6.0);
    centres.erase(std::remove_if(centres.begin(), centres.end(), [&](double c) { return c * edge >= fs / 2; }), centres.end());
    bandBlock = powerOfTwoBelow(static_cast<size_t>(fs));
    raw.clear();

    const char letter = "ACZ"[std::clamp(kind, 0, 2)];
    const char speed = timeWeighting == 0 ? 'F' : 'S';
    const std::string base = std::string("L") + letter;
    const auto decibels = Unit("dB", -1, "decibels", "sound pressure level");
    time.setDescriptor(timeDescriptor(reader.origin, 100'000));
    level.setDescriptor(valueDescriptor((base + speed).c_str(), decibels));
    leq.setDescriptor(valueDescriptor((base + "eq").c_str(), decibels));
    maximum.setDescriptor(valueDescriptor((base + speed + "max").c_str(), decibels));
    bandsTime.setDescriptor(timeDescriptor(reader.origin, 1'000'000));
    if (!centres.empty())
    {
        auto labels = List<IFloat>();
        for (const double c : centres)
            labels.pushBack(std::round(c * 10) / 10);
        const auto dimension = DimensionBuilder().setRule(ListDimensionRule(labels)).setUnit(Unit("Hz", -1, "hertz", "frequency")).setName("Band").build();
        bands.setDescriptor(DataDescriptorBuilder()
                                .setSampleType(SampleType::Float32)
                                .setDimensions(List<IDimension>(dimension))
                                .setUnit(decibels)
                                .setName("Bands")
                                .build());
    }
}

void SoundLevelMeterFb::emitLevels(int64_t end)
{
    const double ref2 = reference * reference;
    const double current = std::max(kFloorDb, 10 * std::log10(std::max(meanSquare, 1e-300) / ref2));
    maxLevel = std::max(maxLevel, current);
    const double equivalent = energyCount > 0 ? std::max(kFloorDb, 10 * std::log10(std::max(energy / energyCount, 1e-300) / ref2)) : kFloorDb;
    const double values[3] = {current, equivalent, maxLevel};
    const auto domainPacket = DataPacket(time.getDescriptor(), 1, end);
    time.sendPacket(domainPacket);
    const SignalConfigPtr outputs[3] = {level, leq, maximum};
    for (int i = 0; i < 3; i++)
    {
        const auto packet = DataPacketWithDomain(domainPacket, outputs[i].getDescriptor(), 1);
        *static_cast<double*>(packet.getRawData()) = values[i];
        outputs[i].sendPacket(packet);
    }
}

void SoundLevelMeterFb::emitBands(double endSeconds)
{
    if (centres.empty() || raw.size() < bandBlock || bandBlock < 256)
        return;
    const size_t n = bandBlock;
    std::vector<std::complex<double>> buffer(n);
    const size_t start = raw.size() - n;
    for (size_t i = 0; i < n; i++)
        buffer[i] = raw[start + i];
    fft(buffer);
    // Parseval: a band's mean square is the sum of its bins' power.
    const double fs = configuredRate;
    const double edge = bandKind == 1 ? std::sqrt(2.0) : std::pow(2.0, 1.0 / 6.0);
    const auto domainPacket = DataPacket(bandsTime.getDescriptor(), 1, std::llround(endSeconds * 1e6));
    const auto packet = DataPacketWithDomain(domainPacket, bands.getDescriptor(), 1);
    auto* out = static_cast<float*>(packet.getRawData());
    for (size_t b = 0; b < centres.size(); b++)
    {
        const double low = centres[b] / edge;
        const double high = centres[b] * edge;
        double power = 0;
        for (size_t k = 1; k < n / 2; k++)
        {
            const double frequency = static_cast<double>(k) * fs / static_cast<double>(n);
            if (frequency >= low && frequency < high)
                power += 2 * std::norm(buffer[k]);
        }
        const double ms = power / (static_cast<double>(n) * static_cast<double>(n));
        out[b] = static_cast<float>(std::max(kFloorDb, 10 * std::log10(std::max(ms, 1e-300) / (reference * reference))));
    }
    bandsTime.sendPacket(domainPacket);
    bands.sendPacket(packet);
}

void SoundLevelMeterFb::onPacketReceived(const InputPortPtr& /*port*/)
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
                configuredVersion = -1;
                setComponentStatus(ComponentStatus::Ok);
            }
            else
            {
                setComponentStatusWithMessage(ComponentStatus::Warning, "The sound level meter reads a microphone signal with a fixed sample rate.");
            }
            continue;
        }
        if (packet.getType() != PacketType::Data || !reader.numbers || reader.sampleRate <= 0)
            continue;
        if (configuredVersion != version || configuredRate != reader.sampleRate)
            configure();
        if (resetRequested.exchange(false))
        {
            energy = energyCount = 0;
            maxLevel = -1e300;
        }
        reader.read(packet.asPtr<IDataPacket>(true), times, values);
        for (size_t i = 0; i < times.size(); i++)
        {
            const int64_t at = std::llround(times[i] * 1e6);
            const int64_t index = floorDivide(at, 100'000);
            if (index != outputIndex)
            {
                if (outputIndex != INT64_MIN)
                {
                    emitLevels((outputIndex + 1) * 100'000);
                    // The bands once a second, at the full second.
                    if (floorDivide(index, 10) != floorDivide(outputIndex, 10))
                        emitBands(static_cast<double>(index / 10 * 10) / 10.0);
                }
                outputIndex = index;
            }
            double weighted = values[i];
            for (auto& section : weighting)
                weighted = section.step(weighted);
            weighted *= weightingGain;
            meanSquare += alpha * (weighted * weighted - meanSquare);
            energy += weighted * weighted;
            energyCount += 1;
            raw.push_back(values[i]);
            if (raw.size() > bandBlock)
                raw.pop_front();
        }
    }
}

}  // namespace daq::modules::universal_renderer
