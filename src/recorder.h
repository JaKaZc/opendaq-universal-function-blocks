// Records the drawing as plot data: a small binary list of lines, filled
// areas, texts and pictures in the plot's coordinates, which openDAQ Studio
// draws with its own graphics at any size. The format is in README.md.
#pragma once

#include "surface.h"

namespace daq::modules::universal_renderer
{

class Recorder final : public Surface
{
public:
    Recorder(int width, int height);

    int width() const override { return w; }
    int height() const override { return h; }

    void clear(Color color) override;
    void polyline(const std::vector<Point>& points, float thickness, Color color, LineStyle style) override;
    void fillUnder(const std::vector<Point>& points, float baseline, Color color, int alpha) override;
    float text(const std::string& value, float x, float top, Color color, bool bold, Align align) override;
    void picture(const Frame& frame, float x, float y, float width, float height) override;

    /// The recording.
    std::vector<uint8_t> bytes() const;

private:
    int w;
    int h;
    Color background;
    uint32_t count = 0;
    std::vector<uint8_t> ops;

    void u8(uint8_t value);
    void u16(uint16_t value);
    void u32(uint32_t value);
    void coordinate(float value);
    void color(Color value);
    void points(const std::vector<Point>& values);
};

}  // namespace daq::modules::universal_renderer
