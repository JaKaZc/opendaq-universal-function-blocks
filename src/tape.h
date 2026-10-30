// Records the drawing as it is called, to draw it again on another surface,
// on another thread. The renderer lays out each plot once on a Tape and plays
// it into plot data at once and into the Picture output's pixels on the
// picture thread, so the picture never holds up the plot.
#pragma once

#include "surface.h"

#include <functional>
#include <memory>

namespace daq::modules::universal_renderer
{

class Tape final : public Surface
{
public:
    Tape(int width, int height);

    int width() const override { return w; }
    int height() const override { return h; }

    void clear(Color color) override;
    void polyline(const std::vector<Point>& points, float thickness, Color color, LineStyle style) override;
    void fillUnder(const std::vector<Point>& points, float baseline, Color color, int alpha) override;
    float text(const std::string& value, float x, float top, Color color, bool bold, Align align) override;
    void picture(const Frame& frame, float x, float y, float width, float height) override;

    /// The pictures drawn, each once, to swap for decoded ones before playing.
    using Pictures = std::function<std::shared_ptr<const Frame>(const std::shared_ptr<const Frame>&)>;

    /// Draws everything recorded on `out`, the pictures as `pictures` gives
    /// them, if given.
    void play(Surface& out, const Pictures& pictures = {}) const;

private:
    enum class Kind
    {
        Clear,
        Polyline,
        Fill,
        Text,
        Picture
    };

    struct Op
    {
        Kind kind;
        Color color;
        std::vector<Point> points;
        // The thickness, the baseline or the text's place, and a picture's
        // rectangle.
        float a = 0;
        float b = 0;
        float c = 0;
        float d = 0;
        LineStyle style = LineStyle::Solid;
        int alpha = 255;
        std::string text;
        bool bold = false;
        Align align = Align::Left;
        std::shared_ptr<const Frame> frame;
    };

    int w;
    int h;
    std::vector<Op> ops;
};

}  // namespace daq::modules::universal_renderer
