#include "signal_reader.h"

#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/reader_utils.h>

#include <algorithm>
#include <cctype>
#include <chrono>

namespace daq::modules::universal_renderer
{

namespace
{

bool isNumber(SampleType type)
{
    switch (type)
    {
        case SampleType::Float32:
        case SampleType::Float64:
        case SampleType::Int8:
        case SampleType::Int16:
        case SampleType::Int32:
        case SampleType::Int64:
        case SampleType::UInt8:
        case SampleType::UInt16:
        case SampleType::UInt32:
        case SampleType::UInt64:
            return true;
        default:
            return false;
    }
}

/// The type of the samples getData() returns, after any post scaling.
SampleType valueTypeOf(const DataDescriptorPtr& descriptor)
{
    const auto scaling = descriptor.getPostScaling();
    if (scaling.assigned())
        return scaling.getOutputSampleType() == ScaledSampleType::Float32 ? SampleType::Float32 : SampleType::Float64;
    return descriptor.getSampleType();
}

}  // namespace

double readNumber(const void* data, SampleType type, size_t i)
{
    switch (type)
    {
        case SampleType::Float32:
            return static_cast<const float*>(data)[i];
        case SampleType::Float64:
            return static_cast<const double*>(data)[i];
        case SampleType::Int8:
            return static_cast<const int8_t*>(data)[i];
        case SampleType::Int16:
            return static_cast<const int16_t*>(data)[i];
        case SampleType::Int32:
            return static_cast<const int32_t*>(data)[i];
        case SampleType::Int64:
            return static_cast<double>(static_cast<const int64_t*>(data)[i]);
        case SampleType::UInt8:
            return static_cast<const uint8_t*>(data)[i];
        case SampleType::UInt16:
            return static_cast<const uint16_t*>(data)[i];
        case SampleType::UInt32:
            return static_cast<const uint32_t*>(data)[i];
        case SampleType::UInt64:
            return static_cast<double>(static_cast<const uint64_t*>(data)[i]);
        default:
            return 0.0;
    }
}

bool SignalReader::event(const PacketPtr& packet)
{
    if (packet.getType() != PacketType::Event)
        return false;
    const auto event = packet.asPtr<IEventPacket>(true);
    if (event.getEventId() != event_packet_id::DATA_DESCRIPTOR_CHANGED)
        return true;
    const DataDescriptorPtr value = event.getParameters().get(event_packet_param::DATA_DESCRIPTOR);
    const DataDescriptorPtr domain = event.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR);
    if (value.assigned())
        valueDescriptor = value;
    if (domain.assigned())
        domainDescriptor = domain;

    numbers = vector = picture = false;
    vectorSize = 0;
    vectorAxis.clear();
    unit.clear();
    if (!valueDescriptor.assigned())
        return true;
    if (valueDescriptor.getUnit().assigned())
        unit = valueDescriptor.getUnit().getSymbol().toStdString();

    if (valueDescriptor.getSampleType() == SampleType::Binary)
    {
        std::string format = unit;
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (format == "jpg")
            format = "jpeg";
        picture = format == "jpeg" || format == "png";
        pictureFormat = format;
    }
    else if (isNumber(valueTypeOf(valueDescriptor)))
    {
        const auto dimensions = valueDescriptor.getDimensions();
        const size_t count = dimensions.assigned() ? dimensions.getCount() : 0;
        if (count == 0)
        {
            numbers = true;
        }
        else if (count == 1)
        {
            vector = true;
            vectorSize = dimensions[0].getSize();
            for (const auto& label : dimensions[0].getLabels())
                vectorAxis.push_back(label.supportsInterface(IFloat::Id) ? static_cast<double>(static_cast<Float>(label))
                                     : label.supportsInterface(IInteger::Id) ? static_cast<double>(static_cast<Int>(label))
                                                                             : static_cast<double>(vectorAxis.size()));
            if (dimensions[0].getUnit().assigned())
                vectorAxisUnit = dimensions[0].getUnit().getSymbol().toStdString();
        }
    }

    tick = 0;
    sampleRate = 0;
    unixTime = false;
    origin.clear();
    if (domainDescriptor.assigned() && domainDescriptor.getTickResolution().assigned())
    {
        const auto resolution = domainDescriptor.getTickResolution();
        tick = static_cast<double>(resolution.getNumerator()) / static_cast<double>(resolution.getDenominator());
        origin = domainDescriptor.getOrigin().assigned() ? domainDescriptor.getOrigin().toStdString() : "";
        const auto domainUnit = domainDescriptor.getUnit();
        if (!origin.empty() && domainUnit.assigned() && domainUnit.getSymbol() == "s")
        {
            const auto epoch = reader::parseEpoch(origin);
            originSeconds = std::chrono::duration<double>(epoch.time_since_epoch()).count();
            unixTime = true;
        }
        const auto rule = domainDescriptor.getRule();
        if (rule.assigned() && rule.getType() == DataRuleType::Linear)
        {
            const int64_t delta = rule.getParameters().get("delta");
            if (delta > 0 && tick > 0)
                sampleRate = 1.0 / (static_cast<double>(delta) * tick);
        }
    }
    return true;
}

void SignalReader::read(const DataPacketPtr& packet, std::vector<double>& times, std::vector<double>& values) const
{
    times.clear();
    values.clear();
    if (!(numbers || vector) || tick <= 0)
        return;
    const auto domainPacket = packet.getDomainPacket();
    const size_t count = packet.getSampleCount();
    const void* data = packet.getData();
    if (!domainPacket.assigned() || count == 0 || !data)
        return;

    const auto domain = domainPacket.getDataDescriptor();
    int64_t reference = 0;
    if (const auto info = domain.getReferenceDomainInfo(); info.assigned())
        if (const auto offset = info.getReferenceDomainOffset(); offset.assigned())
            reference = offset;
    const auto rule = domain.getRule();
    times.resize(count);
    if (rule.getType() == DataRuleType::Linear)
    {
        const auto parameters = rule.getParameters();
        const int64_t delta = parameters.get("delta");
        const int64_t start = parameters.get("start");
        const auto offset = domainPacket.getOffset();
        const int64_t first = reference + start + (offset.assigned() ? offset.getIntValue() : 0);
        for (size_t i = 0; i < count; i++)
            times[i] = static_cast<double>(first + delta * static_cast<int64_t>(i)) * tick;
    }
    else
    {
        const void* domainData = domainPacket.getData();
        if (!domainData || !isNumber(domain.getSampleType()))
        {
            times.clear();
            return;
        }
        for (size_t i = 0; i < count; i++)
            times[i] = (readNumber(domainData, domain.getSampleType(), i) + static_cast<double>(reference)) * tick;
    }

    const SampleType type = valueTypeOf(packet.getDataDescriptor());
    const size_t perSample = vector ? vectorSize : 1;
    values.resize(count * perSample);
    for (size_t i = 0; i < values.size(); i++)
        values[i] = readNumber(data, type, i);
}

}  // namespace daq::modules::universal_renderer
