#include "recorder.h"

#include <algorithm>
#include <cmath>

namespace daq::modules::universal_renderer
{

namespace
{

// The plot data's operations.
enum Op : uint8_t
{
    kPolyline = 1,
    kFill = 2,
    kText = 3,
    kPicture = 4,
};

}  // namespace

Recorder::Recorder(int width, int height)
    : w(width)
    , h(height)
{
}

void Recorder::u8(uint8_t value)
{
    ops.push_back(value);
}

void Recorder::u16(uint16_t value)
{
    ops.push_back(static_cast<uint8_t>(value));
    ops.push_back(static_cast<uint8_t>(value >> 8));
}

void Recorder::u32(uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        ops.push_back(static_cast<uint8_t>(value >> shift));
}

// Coordinates are kept to an eighth of a pixel, in a signed 16-bit number.
void Recorder::coordinate(float value)
{
    const float clamped = std::clamp(value * 8.0f, -32768.0f, 32767.0f);
    u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(clamped))));
}

void Recorder::color(Color value)
{
    u8(value.r);
    u8(value.g);
    u8(value.b);
}

void Recorder::points(const std::vector<Point>& values)
{
    u32(static_cast<uint32_t>(values.size()));
    for (const auto& point : values)
    {
        coordinate(point.x);
        coordinate(point.y);
    }
}

void Recorder::clear(Color color)
{
    background = color;
    ops.clear();
    count = 0;
}

void Recorder::polyline(const std::vector<Point>& values, float thickness, Color value, LineStyle style)
{
    if (values.empty())
        return;
    u8(kPolyline);
    color(value);
    u8(style == LineStyle::Dash ? 1 : 0);
    u16(static_cast<uint16_t>(std::clamp(thickness * 16.0f, 0.0f, 65535.0f)));
    points(values);
    count++;
}

void Recorder::fillUnder(const std::vector<Point>& values, float baseline, Color value, int alpha)
{
    if (values.size() < 2)
        return;
    u8(kFill);
    color(value);
    u8(static_cast<uint8_t>(std::clamp(alpha, 0, 255)));
    coordinate(baseline);
    points(values);
    count++;
}

float Recorder::text(const std::string& value, float x, float top, Color colorValue, bool bold, Align align)
{
    const size_t length = std::min<size_t>(value.size(), 65535);
    u8(kText);
    color(colorValue);
    u8(static_cast<uint8_t>((bold ? 1 : 0) | (static_cast<int>(align) << 1)));
    coordinate(x);
    coordinate(top);
    u16(static_cast<uint16_t>(length));
    ops.insert(ops.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
    count++;
    return textWidth(value, bold);
}

void Recorder::picture(const Frame& frame, float x, float y, float width, float height)
{
    // The picture travels as it came; the viewer decodes it, once per id.
    u8(kPicture);
    color({});
    u8(frame.format == "png" ? 2 : 1);
    u32(frame.id);
    coordinate(x);
    coordinate(y);
    coordinate(width);
    coordinate(height);
    u32(static_cast<uint32_t>(frame.bytes.size()));
    ops.insert(ops.end(), frame.bytes.begin(), frame.bytes.end());
    count++;
}

std::vector<uint8_t> Recorder::bytes() const
{
    std::vector<uint8_t> out = {'O', 'D', 'P', 'L', 1, 0, 0, 0};
    const auto put16 = [&](uint32_t value)
    {
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
    };
    put16(static_cast<uint32_t>(w));
    put16(static_cast<uint32_t>(h));
    out.insert(out.end(), {background.r, background.g, background.b, 0});
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(count >> shift));
    out.insert(out.end(), ops.begin(), ops.end());
    return out;
}

}  // namespace daq::modules::universal_renderer
