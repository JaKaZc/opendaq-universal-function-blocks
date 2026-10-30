// Reading an input port's packets as numbers with times, for the Level,
// Spectrum and Dashboard function blocks.
#pragma once

#include <opendaq/opendaq.h>

#include <string>
#include <vector>

namespace daq::modules::universal_renderer
{

/// What an input signal is, from its descriptors, and its samples.
struct SignalReader
{
    DataDescriptorPtr valueDescriptor;
    DataDescriptorPtr domainDescriptor;

    /// Numbers, one per sample, or a fixed number per sample (a vector, such
    /// as a spectrum), or pictures (Binary with a jpeg or png unit).
    bool numbers = false;
    bool vector = false;
    bool picture = false;
    size_t vectorSize = 0;
    std::vector<double> vectorAxis;  // the value of each element, such as its frequency
    std::string vectorAxisUnit;
    std::string pictureFormat;        // "jpeg" or "png"

    std::string unit;
    /// Seconds per domain tick, and the domain's origin, as text and as Unix
    /// seconds when it is a time origin.
    double tick = 0;
    std::string origin;
    double originSeconds = 0;
    bool unixTime = false;
    /// Samples per second, for a domain with a fixed rate; 0 otherwise.
    double sampleRate = 0;

    /// Takes a descriptor change. Returns true when the packet was one.
    bool event(const PacketPtr& packet);

    /// The samples of a data packet: each one's time in seconds since the
    /// domain's origin, and its values (vectorSize per sample for a vector).
    void read(const DataPacketPtr& packet, std::vector<double>& times, std::vector<double>& values) const;
};

/// Reads a number of any numeric sample type.
double readNumber(const void* data, SampleType type, size_t index);

}  // namespace daq::modules::universal_renderer
