#include "canvas.h"

#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace daq::modules::universal_renderer
{

namespace
{

constexpr float kDashOn = 6.0f;
constexpr float kDashOff = 4.0f;

}  // namespace

Canvas::Canvas(int width, int height)
    : w(width)
    , h(height)
    , rgb(static_cast<size_t>(width) * height * 3)
    , mask(static_cast<size_t>(width) * height)
{
}

void Canvas::clear(Color color)
{
    // The first row pixel by pixel, the others copied from it.
    const size_t stride = static_cast<size_t>(w) * 3;
    if (rgb.empty())
        return;
    for (size_t i = 0; i < stride; i += 3)
    {
        rgb[i] = color.r;
        rgb[i + 1] = color.g;
        rgb[i + 2] = color.b;
    }
    for (size_t row = stride; row < rgb.size(); row += stride)
        std::memcpy(&rgb[row], rgb.data(), stride);
}

void Canvas::blend(int x, int y, Color color, int alpha)
{
    if (x < 0 || y < 0 || x >= w || y >= h || alpha <= 0)
        return;
    uint8_t* pixel = &rgb[(static_cast<size_t>(y) * w + x) * 3];
    if (alpha >= 255)
    {
        pixel[0] = color.r;
        pixel[1] = color.g;
        pixel[2] = color.b;
        return;
    }
    pixel[0] = static_cast<uint8_t>(pixel[0] + (color.r - pixel[0]) * alpha / 255);
    pixel[1] = static_cast<uint8_t>(pixel[1] + (color.g - pixel[1]) * alpha / 255);
    pixel[2] = static_cast<uint8_t>(pixel[2] + (color.b - pixel[2]) * alpha / 255);
}

// Coverage of a capsule around the segment: full inside the line, fading over
// half a pixel at its edge. Each row is scanned only where the segment can
// reach it, so long lines cost their length, not their bounding box.
void Canvas::cover(Point from, Point to, float radius)
{
    const float reach = radius + 1.0f;
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length2 = dx * dx + dy * dy;
    const int first = std::max(0, static_cast<int>(std::floor(std::min(from.y, to.y) - reach)));
    const int last = std::min(h - 1, static_cast<int>(std::ceil(std::max(from.y, to.y) + reach)));
    for (int y = first; y <= last; y++)
    {
        const float cy = y + 0.5f;
        float low = 0.0f;
        float high = 1.0f;
        if (std::abs(dy) > 1e-6f)
        {
            low = (cy - reach - from.y) / dy;
            high = (cy + reach - from.y) / dy;
            if (low > high)
                std::swap(low, high);
            low = std::max(low, 0.0f);
            high = std::min(high, 1.0f);
            if (low > high)
                continue;
        }
        else if (std::abs(from.y - cy) > reach)
        {
            continue;
        }
        const float xa = from.x + dx * low;
        const float xb = from.x + dx * high;
        const int left = std::max(0, static_cast<int>(std::floor(std::min(xa, xb) - reach)));
        const int right = std::min(w - 1, static_cast<int>(std::ceil(std::max(xa, xb) + reach)));
        if (left > right)
            continue;
        uint8_t* row = &mask[static_cast<size_t>(y) * w];
        for (int x = left; x <= right; x++)
        {
            const float cx = x + 0.5f;
            float t = length2 > 0 ? ((cx - from.x) * dx + (cy - from.y) * dy) / length2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float ex = cx - (from.x + t * dx);
            const float ey = cy - (from.y + t * dy);
            const float coverage = std::clamp(radius + 0.5f - std::sqrt(ex * ex + ey * ey), 0.0f, 1.0f);
            const auto value = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
            if (value > row[x])
                row[x] = value;
        }
        maskTop = std::min(maskTop, y);
        maskBottom = std::max(maskBottom, y);
        maskLeft = std::min(maskLeft, left);
        maskRight = std::max(maskRight, right);
    }
}

void Canvas::composite(Color color)
{
    for (int y = maskTop; y <= maskBottom; y++)
    {
        uint8_t* row = &mask[static_cast<size_t>(y) * w];
        for (int x = maskLeft; x <= maskRight; x++)
        {
            if (row[x])
            {
                blend(x, y, color, row[x]);
                row[x] = 0;
            }
        }
    }
    maskTop = h;
    maskBottom = -1;
    maskLeft = w;
    maskRight = -1;
}

void Canvas::polyline(const std::vector<Point>& points, float thickness, Color color, LineStyle style)
{
    if (points.empty())
        return;
    maskTop = h;
    maskBottom = -1;
    maskLeft = w;
    maskRight = -1;
    const float radius = std::max(thickness, 0.5f) / 2.0f;
    if (points.size() == 1)
        cover(points[0], points[0], radius);
    // Where the dash pattern is, carried from one segment to the next.
    float phase = 0.0f;
    for (size_t i = 1; i < points.size(); i++)
    {
        const Point a = points[i - 1];
        const Point b = points[i];
        if (style == LineStyle::Solid)
        {
            cover(a, b, radius);
            continue;
        }
        const float length = std::hypot(b.x - a.x, b.y - a.y);
        float done = 0.0f;
        while (done < length)
        {
            const bool on = phase < kDashOn;
            const float left = on ? kDashOn - phase : kDashOn + kDashOff - phase;
            const float step = std::min(left, length - done);
            if (on && length > 0)
            {
                const float t0 = done / length;
                const float t1 = (done + step) / length;
                cover({a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0}, {a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1}, radius);
            }
            done += step;
            phase = std::fmod(phase + step, kDashOn + kDashOff);
        }
    }
    composite(color);
}

void Canvas::fillColumn(int x, float top, float bottom, Color color, int alpha)
{
    const int from = std::max(0, static_cast<int>(std::lround(top)));
    const int to = std::min(h - 1, static_cast<int>(std::lround(bottom)));
    for (int y = from; y <= to; y++)
        blend(x, y, color, alpha);
}

void Canvas::fillUnder(const std::vector<Point>& points, float baseline, Color color, int alpha)
{
    if (points.size() < 2)
        return;
    // The highest point of the line in each pixel column, from the segments
    // that cross it.
    std::vector<float> tops;
    int first = 0;
    for (size_t i = 1; i < points.size(); i++)
    {
        Point a = points[i - 1];
        Point b = points[i];
        if (a.x > b.x)
            std::swap(a, b);
        const int from = std::max(0, static_cast<int>(std::ceil(a.x)));
        const int to = std::min(w - 1, static_cast<int>(std::floor(b.x)));
        for (int x = from; x <= to; x++)
        {
            const float y = b.x > a.x ? a.y + (b.y - a.y) * (static_cast<float>(x) - a.x) / (b.x - a.x) : std::min(a.y, b.y);
            if (tops.empty())
                first = x;
            if (x < first)
                continue;
            const size_t at = static_cast<size_t>(x - first);
            if (at >= tops.size())
                tops.resize(at + 1, std::numeric_limits<float>::infinity());
            tops[at] = std::min(tops[at], y);
        }
    }
    for (size_t i = 0; i < tops.size(); i++)
        if (std::isfinite(tops[i]))
            fillColumn(first + static_cast<int>(i), tops[i], baseline, color, alpha);
}

float Canvas::text(const std::string& value, float x, float top, Color color, bool bold, Align align)
{
    const float width = textWidth(value, bold);
    const float left = align == Align::Center ? x - width / 2.0f : align == Align::Right ? x - width : x;
    const int ascent = bold ? font::kBoldAscent : font::kRegularAscent;
    const uint8_t* glyphPixels = bold ? font::kBoldPixels : font::kRegularPixels;
    const int baseline = static_cast<int>(std::lround(top)) + ascent;
    float pen = left;
    for (size_t i = 0; i < value.size();)
    {
        const auto& glyph = glyphFor(nextCodePoint(value, i), bold);
        const int x0 = static_cast<int>(std::lround(pen)) + glyph.left;
        const int y0 = baseline + glyph.top;
        for (int gy = 0; gy < glyph.height; gy++)
            for (int gx = 0; gx < glyph.width; gx++)
                blend(x0 + gx, y0 + gy, color, glyphPixels[glyph.offset + static_cast<size_t>(gy) * glyph.width + gx]);
        pen += glyph.advance / 64.0f;
    }
    return width;
}

// Each pixel of the rectangle takes the picture's colour at that spot,
// blended from its four nearest pixels, so a scaled picture stays smooth.
void Canvas::picture(const Frame& frame, float x, float y, float width, float height)
{
    if (frame.decodeFailed)
    {
        // Progressive JPEG and PNG pictures are drawn only in the plot data.
        text("This picture is shown only in openDAQ Studio.", x + width / 2.0f, y + height / 2.0f - 10.0f, {150, 150, 150}, false,
             Align::Center);
        return;
    }
    if (frame.rgb.empty() || frame.width <= 0 || frame.height <= 0 || width <= 0 || height <= 0)
        return;
    const int left = std::max(0, static_cast<int>(std::lround(x)));
    const int right = std::min(w, static_cast<int>(std::lround(x + width)));
    const int first = std::max(0, static_cast<int>(std::lround(y)));
    const int last = std::min(h, static_cast<int>(std::lround(y + height)));
    const float sx = static_cast<float>(frame.width) / width;
    const float sy = static_cast<float>(frame.height) / height;
    for (int py = first; py < last; py++)
    {
        const float fy = std::clamp((py + 0.5f - y) * sy - 0.5f, 0.0f, static_cast<float>(frame.height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, frame.height - 1);
        const float wy = fy - y0;
        uint8_t* out = &rgb[(static_cast<size_t>(py) * w + left) * 3];
        for (int px = left; px < right; px++, out += 3)
        {
            const float fx = std::clamp((px + 0.5f - x) * sx - 0.5f, 0.0f, static_cast<float>(frame.width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, frame.width - 1);
            const float wx = fx - x0;
            const uint8_t* a = &frame.rgb[(static_cast<size_t>(y0) * frame.width + x0) * 3];
            const uint8_t* b = &frame.rgb[(static_cast<size_t>(y0) * frame.width + x1) * 3];
            const uint8_t* c = &frame.rgb[(static_cast<size_t>(y1) * frame.width + x0) * 3];
            const uint8_t* d = &frame.rgb[(static_cast<size_t>(y1) * frame.width + x1) * 3];
            for (int k = 0; k < 3; k++)
            {
                const float top = a[k] + (b[k] - a[k]) * wx;
                const float bottom = c[k] + (d[k] - c[k]) * wx;
                out[k] = static_cast<uint8_t>(top + (bottom - top) * wy + 0.5f);
            }
        }
    }
}

}  // namespace daq::modules::universal_renderer
