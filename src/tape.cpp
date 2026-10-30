#include "tape.h"

namespace daq::modules::universal_renderer
{

Tape::Tape(int width, int height)
    : w(width)
    , h(height)
{
}

void Tape::clear(Color color)
{
    ops.clear();
    Op op{Kind::Clear};
    op.color = color;
    ops.push_back(std::move(op));
}

void Tape::polyline(const std::vector<Point>& points, float thickness, Color color, LineStyle style)
{
    Op op{Kind::Polyline};
    op.points = points;
    op.a = thickness;
    op.color = color;
    op.style = style;
    ops.push_back(std::move(op));
}

void Tape::fillUnder(const std::vector<Point>& points, float baseline, Color color, int alpha)
{
    Op op{Kind::Fill};
    op.points = points;
    op.a = baseline;
    op.color = color;
    op.alpha = alpha;
    ops.push_back(std::move(op));
}

float Tape::text(const std::string& value, float x, float top, Color color, bool bold, Align align)
{
    Op op{Kind::Text};
    op.text = value;
    op.a = x;
    op.b = top;
    op.color = color;
    op.bold = bold;
    op.align = align;
    ops.push_back(std::move(op));
    return textWidth(value, bold);
}

void Tape::picture(const Frame& frame, float x, float y, float width, float height)
{
    // The input's frame is kept, not copied; one made elsewhere is copied.
    Op op{Kind::Picture};
    op.frame = frame.weak_from_this().lock();
    if (!op.frame)
        op.frame = std::make_shared<const Frame>(frame);
    op.a = x;
    op.b = y;
    op.c = width;
    op.d = height;
    ops.push_back(std::move(op));
}

void Tape::play(Surface& out, const Pictures& pictures) const
{
    for (const auto& op : ops)
    {
        switch (op.kind)
        {
            case Kind::Clear:
                out.clear(op.color);
                break;
            case Kind::Polyline:
                out.polyline(op.points, op.a, op.color, op.style);
                break;
            case Kind::Fill:
                out.fillUnder(op.points, op.a, op.color, op.alpha);
                break;
            case Kind::Text:
                out.text(op.text, op.a, op.b, op.color, op.bold, op.align);
                break;
            case Kind::Picture:
            {
                const auto frame = pictures ? pictures(op.frame) : op.frame;
                if (frame)
                    out.picture(*frame, op.a, op.b, op.c, op.d);
                break;
            }
        }
    }
}

}  // namespace daq::modules::universal_renderer
