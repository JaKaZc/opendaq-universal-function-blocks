// Draws pixels: anti-aliased lines, text and pictures on an RGB picture, for
// the Picture output.
#pragma once

#include "surface.h"

namespace daq::modules::universal_renderer
{

class Canvas final : public Surface
{
public:
    Canvas(int width, int height);

    int width() const override { return w; }
    int height() const override { return h; }

    /// The picture, three bytes a pixel, row by row from the top.
    const std::vector<uint8_t>& pixels() const { return rgb; }

    void clear(Color color) override;

    /// The joints are drawn once, so a dense line is as bright as a sparse one.
    void polyline(const std::vector<Point>& points, float thickness, Color color, LineStyle style) override;
    void fillUnder(const std::vector<Point>& points, float baseline, Color color, int alpha) override;
    float text(const std::string& value, float x, float top, Color color, bool bold, Align align) override;

    /// Draws the frame's decoded pixels; a frame not decoded is left out.
    void picture(const Frame& frame, float x, float y, float width, float height) override;

private:
    int w;
    int h;
    std::vector<uint8_t> rgb;

    // Coverage of the line being drawn, and the rows and columns it touched.
    std::vector<uint8_t> mask;
    int maskTop = 0;
    int maskBottom = -1;
    int maskLeft = 0;
    int maskRight = -1;

    void cover(Point from, Point to, float radius);
    void composite(Color color);
    void blend(int x, int y, Color color, int alpha);
    void fillColumn(int x, float top, float bottom, Color color, int alpha);
};

}  // namespace daq::modules::universal_renderer
