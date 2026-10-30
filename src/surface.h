// What the renderer draws on.
//
// The renderer lays out its plot once and draws it on two surfaces: a
// Recorder, which writes plot data for openDAQ Studio to draw at any size,
// and a Canvas, which draws pixels for the Picture output that any client
// shows. Both are its own, so the function block needs no window, no graphics
// driver and no drawing library, and draws the same on a phone as on a
// desktop.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daq::modules::universal_renderer
{

struct Color
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct Point
{
    float x = 0;
    float y = 0;
};

enum class LineStyle
{
    Solid,
    Dash
};

enum class Align
{
    Left,
    Center,
    Right
};

/// A picture an input delivered, such as a camera's: its bytes as they came,
/// and, for the Picture output, its pixels. Frames are shared, so a recorded
/// plot keeps those it draws.
struct Frame : std::enable_shared_from_this<Frame>
{
    /// A new number for each new picture, so a viewer decodes each once.
    uint32_t id = 0;
    /// "jpeg" or "png".
    std::string format;
    std::vector<uint8_t> bytes;
    /// The size in pixels, from the signal's metadata or the decoded picture.
    int width = 0;
    int height = 0;
    /// Three bytes a pixel, row by row; empty until decoded.
    std::vector<uint8_t> rgb;
    bool decodeFailed = false;
};

class Surface
{
public:
    virtual ~Surface() = default;

    virtual int width() const = 0;
    virtual int height() const = 0;

    /// Starts over, on a background of this color.
    virtual void clear(Color color) = 0;

    /// A line through the points, `thickness` pixels wide.
    virtual void polyline(const std::vector<Point>& points, float thickness, Color color, LineStyle style = LineStyle::Solid) = 0;
    void line(Point from, Point to, float thickness, Color color, LineStyle style = LineStyle::Solid)
    {
        polyline({from, to}, thickness, color, style);
    }

    /// Fills below a line down to a level baseline, `alpha` of 255 opaque,
    /// which hides what was drawn behind it.
    virtual void fillUnder(const std::vector<Point>& points, float baseline, Color color, int alpha) = 0;

    /// Draws UTF-8 text with the top of its line at `top`, starting, centred
    /// or ending at `x`, and returns its width.
    virtual float text(const std::string& value, float x, float top, Color color, bool bold = false, Align align = Align::Left) = 0;

    /// Draws a picture into the rectangle, which keeps its proportions.
    virtual void picture(const Frame& frame, float x, float y, float width, float height) = 0;

    /// Text sizes in DejaVu Sans at 16 pixels, the font the pictures use and
    /// the viewers draw with.
    static float textWidth(const std::string& value, bool bold = false);
    static float lineHeight();
};

}  // namespace daq::modules::universal_renderer
