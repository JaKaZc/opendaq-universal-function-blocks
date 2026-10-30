#include "renderer.h"

#include "jpeg.h"
#include "png.h"


#include <coreobjects/callable_info_factory.h>
#include <coreobjects/eval_value_factory.h>
#include <opendaq/component_type_private.h>
#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/reader_utils.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <type_traits>

namespace daq::modules::universal_renderer
{

namespace
{

constexpr auto kHeartbeat = std::chrono::seconds(1);
constexpr size_t kMaxSamples = 2'000'000;
constexpr float kLabelHeight = 40.0f;

const Color kBackground{0, 0, 0};
const Color kAxis{150, 150, 150};

// The reference renderer's colors, by input.
Color colorOf(size_t index)
{
    static const Color colors[] = {{255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {255, 0, 255}, {0, 255, 255}, {0, 0, 255}};
    return colors[index % 6];
}

void sizeOf(int resolution, int& width, int& height)
{
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1920, 1080}, {2560, 1440}};
    const int i = std::clamp(resolution, 0, 5);
    width = sizes[i][0];
    height = sizes[i][1];
}

int64_t unixMicroseconds()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

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

double read(const void* data, SampleType type, size_t i)
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

std::string format(const char* pattern, double value)
{
    char text[64];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

/// A moment as the phone or computer shows it, to the millisecond.
std::string localTime(double seconds)
{
    const double whole = std::floor(seconds);
    const auto time = static_cast<std::time_t>(whole);
    const int milliseconds = std::clamp(static_cast<int>((seconds - whole) * 1000.0), 0, 999);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[48];
    const size_t length = std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    std::snprintf(text + length, sizeof(text) - length, ".%03d", milliseconds);
    return text;
}

// ---------------------------------------------------------------- drawing

struct Item
{
    const Trace* trace;
    std::string caption;
    Color color;
    size_t index;
    /// Where the time axis ends: the newest sample, or, while data flows on
    /// a clock time axis, the clock less the signal's usual delay.
    double end;
};

struct Window
{
    double first = 0.0;
    double last = 0.0;
};

struct Lane
{
    float left;
    float top;
    float right;
    float bottom;
};

struct Range
{
    double low = 0.0;
    double high = 1.0;
};

struct Problems
{
    ComponentStatus status = ComponentStatus::Ok;
    std::string message;

    void add(ComponentStatus level, const std::string& text)
    {
        if (static_cast<int>(level) > static_cast<int>(status))
        {
            status = level;
            message = text;
        }
    }
};

/// The indices of a vector signal's elements that are drawn: all of them, or
/// the custom 2D range.
void vectorSpan(const Trace& trace, const Settings& settings, size_t& offset, size_t& count)
{
    offset = 0;
    count = trace.vectorValues.empty() ? trace.vectorLabels.size() : trace.vectorValues.size();
    if (settings.useCustom2dRangeValue && settings.custom2dMinRange < settings.custom2dMaxRange)
    {
        if (settings.custom2dMinRange >= 0 && static_cast<size_t>(settings.custom2dMinRange) < count)
            offset = static_cast<size_t>(settings.custom2dMinRange);
        if (settings.custom2dMaxRange >= 0 && static_cast<size_t>(settings.custom2dMaxRange) + 1 <= count)
            count = static_cast<size_t>(settings.custom2dMaxRange) + 1;
        count -= offset;
    }
}

/// The value range of a signal: the custom one, the one its descriptor
/// states, or, without either, the range of what is on screen.
Range rangeOf(const Trace& trace, const Window& window, const Settings& settings)
{
    if (settings.useCustomMinMaxValue)
        return {settings.customMinValue, settings.customMaxValue};
    if (trace.hasValueRange)
        return {trace.rangeLow, trace.rangeHigh};
    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    if (trace.vector)
    {
        size_t offset;
        size_t count;
        vectorSpan(trace, settings, offset, count);
        for (size_t i = offset; i < offset + count && i < trace.vectorValues.size(); i++)
        {
            low = std::min(low, trace.vectorValues[i]);
            high = std::max(high, trace.vectorValues[i]);
        }
    }
    else
    {
        const auto begin = std::lower_bound(trace.samples.begin(), trace.samples.end(), window.first,
                                            [](const Sample& sample, double t) { return sample.t < t; });
        for (auto it = begin; it != trace.samples.end(); ++it)
        {
            if (!std::isfinite(it->value))
                continue;
            low = std::min(low, it->value);
            high = std::max(high, it->value);
        }
    }
    if (!(low <= high))
        return {0.0, 1.0};
    if (high - low < 1e-12)
    {
        const double pad = std::max(std::abs(high) * 0.1, 0.5);
        return {low - pad, high + pad};
    }
    const double pad = (high - low) * 0.05;
    return {low - pad, high + pad};
}

class Plot
{
public:
    Plot(Surface& out, const Settings& settings)
        : out(out)
        , settings(settings)
    {
    }

    void draw(const std::vector<Item>& all, Problems& problems)
    {
        out.clear(kBackground);
        if (all.empty())
        {
            const std::string text = "Connect a signal to an input port.";
            out.text(text, out.width() / 2.0f, out.height() / 2.0f - 10.0f, kAxis, false, Align::Center);
            return;
        }

        // Pictures, such as a camera's, sit side by side in the lower half,
        // or fill the plot when nothing else is connected; the charts take
        // the rest.
        std::vector<Item> items;
        std::vector<const Item*> pictureItems;
        for (const auto& item : all)
        {
            if (item.trace->image)
                pictureItems.push_back(&item);
            else
                items.push_back(item);
        }
        areaHeight = static_cast<float>(out.height());
        if (!pictureItems.empty())
        {
            const float band = items.empty() ? areaHeight : areaHeight / 2.0f;
            pictures(pictureItems, areaHeight - band, band);
            areaHeight -= band;
        }
        if (items.empty())
            return;
        if (settings.view3D)
        {
            draw3D(items, problems);
            return;
        }

        bool singleXAxis = settings.singleXAxis;
        std::vector<Window> windows(items.size());
        for (size_t i = 0; i < items.size(); i++)
        {
            const auto& samples = items[i].trace->samples;
            windows[i].last = samples.empty() ? 0.0 : items[i].end;
            windows[i].first = windows[i].last - settings.duration;
        }
        if (singleXAxis)
            singleXAxis = shareXAxis(items, windows, problems);
        const bool oneChart = singleXAxis && settings.singleYAxis;

        const float width = static_cast<float>(out.width());
        const float height = areaHeight;
        if (oneChart)
        {
            const Lane lane{75.0f, kLabelHeight, width - 25.0f, height - kLabelHeight};
            Range range{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
            for (size_t i = 0; i < items.size(); i++)
            {
                const auto own = rangeOf(*items[i].trace, windows[i], settings);
                range.low = std::min(range.low, own.low);
                range.high = std::max(range.high, own.high);
            }
            axis(lane, items.back(), windows.back(), range, true, false);
            titles(items);
            for (size_t i = 0; i < items.size(); i++)
                signal(lane, items[i], windows[i], range, i, true);
            return;
        }

        const float laneHeight = (height - (singleXAxis ? kLabelHeight : 0.0f)) / static_cast<float>(items.size());
        for (size_t i = 0; i < items.size(); i++)
        {
            const float top = laneHeight * static_cast<float>(i);
            const Lane lane{75.0f, top + kLabelHeight, width - 25.0f, top + laneHeight - (singleXAxis ? 0.0f : kLabelHeight)};
            const auto range = rangeOf(*items[i].trace, windows[i], settings);
            axis(lane, items[i], windows[i], range, !singleXAxis || i + 1 == items.size(), true);
            signal(lane, items[i], windows[i], range, i, false);
        }
    }

private:
    Surface& out;
    const Settings& settings;
    // The height the charts take, above any pictures.
    float areaHeight = 0.0f;

    void pictures(const std::vector<const Item*>& items, float top, float height)
    {
        constexpr float gap = 16.0f;
        const float width = static_cast<float>(out.width());
        const float cell = (width - 50.0f - gap * static_cast<float>(items.size() - 1)) / static_cast<float>(items.size());
        const float areaTop = top + 36.0f;
        const float areaHeightLeft = height - 46.0f;
        for (size_t i = 0; i < items.size(); i++)
        {
            const Item& item = *items[i];
            const float left = 25.0f + static_cast<float>(i) * (cell + gap);
            out.text(item.caption, left + cell / 2.0f, top + 10.0f, item.color, true, Align::Center);
            const auto& frame = item.trace->frame;
            if (!frame || frame->bytes.empty())
            {
                out.text("Waiting for the first picture", left + cell / 2.0f, areaTop + areaHeightLeft / 2.0f - 10.0f, kAxis, false,
                         Align::Center);
                continue;
            }
            const float aspect =
                frame->width > 0 && frame->height > 0 ? static_cast<float>(frame->width) / static_cast<float>(frame->height) : 4.0f / 3.0f;
            float w = cell;
            float h = cell / aspect;
            if (h > areaHeightLeft)
            {
                h = areaHeightLeft;
                w = h * aspect;
            }
            out.picture(*frame, left + (cell - w) / 2.0f, areaTop + (areaHeightLeft - h) / 2.0f, w, h);
        }
    }

    /// Puts the signals on one time axis, ending at the newest sample of any,
    /// if they can share one; as the reference renderer, otherwise it reports
    /// why and draws each on its own.
    bool shareXAxis(const std::vector<Item>& items, std::vector<Window>& windows, Problems& problems)
    {
        const Trace& first = *items.front().trace;
        for (const auto& item : items)
        {
            if (item.trace->vector != first.vector)
            {
                problems.add(ComponentStatus::Warning,
                             "Renderer has multiple input signals with different dimension. Property SingleXAxis is turned off");
                return false;
            }
        }
        if (first.vector)
            return true;
        double last = -std::numeric_limits<double>::infinity();
        for (const auto& item : items)
        {
            const Trace& trace = *item.trace;
            if (trace.hasTimeOrigin != first.hasTimeOrigin)
            {
                problems.add(ComponentStatus::Error, "Unable to configure single X axis: Time origin set on some signals, but not all of them");
                return false;
            }
            if (!first.hasTimeOrigin && trace.domainUnit != first.domainUnit)
            {
                problems.add(ComponentStatus::Error, "Unable to configure single X axis: Domain unit not equal");
                return false;
            }
            if (!first.hasTimeOrigin && trace.domainQuantity != first.domainQuantity)
            {
                problems.add(ComponentStatus::Error, "Unable to configure single X axis: Domain quantity not equal");
                return false;
            }
            if (!trace.samples.empty())
                last = std::max(last, item.end);
        }
        if (std::isfinite(last))
            for (auto& window : windows)
                window = {last - settings.duration, last};
        return true;
    }

    float yOf(const Lane& lane, const Range& range, double value) const
    {
        const double span = range.high - range.low;
        const double y = lane.bottom - (span != 0.0 ? (value - range.low) / span : 0.5) * (lane.bottom - lane.top);
        if (!std::isfinite(y))
            return lane.bottom;
        return static_cast<float>(std::clamp<double>(y, lane.top, lane.bottom));
    }

    void axis(const Lane& lane, const Item& item, const Window& window, const Range& range, bool drawXLabels, bool title)
    {
        const Trace& trace = *item.trace;
        const float thickness = settings.lineThickness;
        constexpr size_t yTicks = 5;
        size_t xStep = 1;
        const auto labels = labelsFor(trace, window, xStep);
        const size_t xTicks = labels.size();

        out.line({lane.left, lane.top}, {lane.left, lane.bottom}, thickness, kAxis);
        out.line({lane.left, lane.bottom}, {lane.right, lane.bottom}, thickness, kAxis);
        const float xSize = lane.right - lane.left;
        const float ySize = lane.bottom - lane.top;
        for (size_t i = 1; i < yTicks; i++)
        {
            const float y = lane.bottom - static_cast<float>(i) * ySize / static_cast<float>(yTicks - 1);
            out.line({lane.left, y}, {lane.right, y}, thickness, kAxis, LineStyle::Dash);
        }
        const auto xAt = [&](size_t i) { return lane.left + (xTicks > 1 ? static_cast<float>(i) * xSize / static_cast<float>(xTicks - 1) : 0.0f); };
        for (size_t i = xStep; i < xTicks; i += xStep)
            out.line({xAt(i), lane.top}, {xAt(i), lane.bottom}, thickness, kAxis, LineStyle::Dash);

        const float lineHeight = Recorder::lineHeight();
        for (size_t i = 0; i < yTicks; i++)
        {
            const float y = lane.bottom - static_cast<float>(i) * ySize / static_cast<float>(yTicks - 1);
            const auto text = format("%.2f", range.low + (range.high - range.low) * static_cast<double>(i) / static_cast<double>(yTicks - 1));
            out.text(text, lane.left - 8.0f, y - lineHeight / 2.0f, kAxis, false, Align::Right);
        }
        if (drawXLabels)
            xLabels(xAt, labels, xStep, lane.bottom + 6.0f);
        if (title)
        {
            out.text(item.caption, (lane.left + lane.right) / 2.0f, lane.top - 26.0f, item.color, true, Align::Center);
        }
    }

    template <typename Items>
    void titles(const Items& items, float top = 8.0f)
    {
        const auto itemOf = [](const auto& entry) -> const Item& {
            if constexpr (std::is_pointer_v<std::decay_t<decltype(entry)>>)
                return *entry;
            else
                return entry;
        };
        // Each title is centred in its own slot, so a viewer's font of a
        // slightly different width does not run one into the next.
        constexpr float gap = 12.0f;
        float total = 0.0f;
        for (const auto& entry : items)
            total += Recorder::textWidth(itemOf(entry).caption, true) + gap;
        float x = (static_cast<float>(out.width()) - total + gap) / 2.0f;
        for (const auto& entry : items)
        {
            const float width = Recorder::textWidth(itemOf(entry).caption, true);
            out.text(itemOf(entry).caption, x + width / 2.0f, top, itemOf(entry).color, true, Align::Center);
            x += width + gap;
        }
    }

    void signal(const Lane& lane, const Item& item, const Window& window, const Range& range, size_t position, bool oneChart)
    {
        const Trace& trace = *item.trace;
        const float xSize = lane.right - lane.left;
        bool haveValue = false;
        double lastValue = 0.0;

        if (trace.vector)
        {
            size_t offset;
            size_t count;
            vectorSpan(trace, settings, offset, count);
            std::vector<Point> points;
            for (size_t i = 0; i < count && offset + i < trace.vectorValues.size(); i++)
            {
                const float x = lane.left + (count > 1 ? static_cast<float>(i) * xSize / static_cast<float>(count - 1) : 0.0f);
                points.push_back({x, yOf(lane, range, trace.vectorValues[offset + i])});
            }
            out.polyline(points, settings.lineThickness, item.color);
            if (!trace.vectorValues.empty())
            {
                lastValue = trace.vectorValues.front();
                haveValue = true;
            }
        }
        else
        {
            const double span = window.last - window.first;
            const auto runs = traceRuns(
                trace, window,
                [&](double t) { return lane.left + static_cast<float>(span > 0 ? (t - window.first) / span : 1.0) * xSize; },
                [&](double value) { return yOf(lane, range, value); }, lastValue, haveValue);
            for (const auto& run : runs)
                out.polyline(run, settings.lineThickness, item.color);
        }

        if (haveValue && settings.showLastValue)
        {
            const std::string text = (oneChart ? item.caption : std::string("Value")) + " = " + format("%.3f", lastValue);
            float top = lane.top - 24.0f;
            if (oneChart)
                top += static_cast<float>(position) * 22.0f;
            out.text(text, lane.right, top, kAxis, false, Align::Right);
        }
    }

    // ---------------------------------------------------------------- 3D

    /// A box drawn in oblique projection: its front face spans left to
    /// right - dx and top + dy to bottom, and depth moves a point dx right
    /// and dy up.
    struct Box
    {
        float left;
        float top;
        float right;
        float bottom;
        float dx;
        float dy;
    };

    /// The point at u along the x axis, v up the value axis and w into the
    /// depth, each from 0 to 1.
    static Point at(const Box& box, double u, double v, double w)
    {
        const float frontWidth = box.right - box.dx - box.left;
        const float frontHeight = box.bottom - box.dy - box.top;
        return {box.left + static_cast<float>(u) * frontWidth + static_cast<float>(w) * box.dx,
                box.bottom - static_cast<float>(v) * frontHeight - static_cast<float>(w) * box.dy};
    }

    static double share(const Range& range, double value)
    {
        const double span = range.high - range.low;
        if (!(span > 0.0) || !std::isfinite(value))
            return 0.5;
        return std::clamp((value - range.low) / span, 0.0, 1.0);
    }

    static Color mix(Color a, Color b, float toB)
    {
        const auto channel = [&](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * toB); };
        return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b)};
    }

    /// The walls and floor of a box, with its grid and its value and x labels.
    void frame(const Box& box, const std::vector<std::string>& labels, size_t xStep, const Range* range)
    {
        const float thickness = settings.lineThickness;
        const size_t xTicks = labels.size();
        constexpr size_t yTicks = 5;
        // The back wall and the left wall, with the value grid.
        for (size_t i = 1; i < yTicks; i++)
        {
            const double v = static_cast<double>(i) / (yTicks - 1);
            out.line(at(box, 0, v, 1), at(box, 1, v, 1), thickness, kAxis, LineStyle::Dash);
            out.line(at(box, 0, v, 0), at(box, 0, v, 1), thickness, kAxis, LineStyle::Dash);
        }
        out.line(at(box, 0, 0, 1), at(box, 0, 1, 1), thickness, kAxis);
        out.line(at(box, 0, 0, 0), at(box, 0, 1, 0), thickness, kAxis);
        // The floor, with the x grid running into the depth.
        for (size_t i = xStep; i + 1 < xTicks; i += xStep)
        {
            const double u = static_cast<double>(i) / (xTicks - 1);
            out.line(at(box, u, 0, 0), at(box, u, 0, 1), thickness, kAxis, LineStyle::Dash);
            out.line(at(box, u, 0, 1), at(box, u, 1, 1), thickness, kAxis, LineStyle::Dash);
        }
        out.line(at(box, 0, 0, 1), at(box, 1, 0, 1), thickness, kAxis);
        out.line(at(box, 1, 0, 0), at(box, 1, 0, 1), thickness, kAxis);
        out.line(at(box, 1, 0, 1), at(box, 1, 1, 1), thickness, kAxis);
        out.line(at(box, 0, 0, 0), at(box, 0, 0, 1), thickness, kAxis);
        out.line(at(box, 0, 0, 0), at(box, 1, 0, 0), thickness, kAxis);

        const float lineHeight = Recorder::lineHeight();
        if (range)
        {
            for (size_t i = 0; i < yTicks; i++)
            {
                const double v = static_cast<double>(i) / (yTicks - 1);
                const auto text = format("%.2f", range->low + (range->high - range->low) * v);
                const Point point = at(box, 0, v, 0);
                out.text(text, point.x - 8.0f, point.y - lineHeight / 2.0f, kAxis, false, Align::Right);
            }
        }
        xLabels([&](size_t i) { return at(box, xTicks > 1 ? static_cast<double>(i) / (xTicks - 1) : 0.0, 0, 0).x; }, labels, xStep,
                box.bottom + 6.0f);
    }

    /// Scalar signals in 3D: one time axis, and each signal a surface on a
    /// plane of its own, the first at the back.
    void scalars3D(const Box& box, const std::vector<const Item*>& items, const std::vector<Window>& windows, float laneTop)
    {
        const size_t count = items.size();
        const bool shared = settings.useCustomMinMaxValue || settings.singleYAxis || count == 1;
        std::vector<Range> ranges;
        Range all{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        for (size_t i = 0; i < count; i++)
        {
            ranges.push_back(rangeOf(*items[i]->trace, windows[i], settings));
            all.low = std::min(all.low, ranges.back().low);
            all.high = std::max(all.high, ranges.back().high);
        }
        if (shared)
            for (auto& range : ranges)
                range = all;

        size_t xStep = 1;
        const auto labels = labelsFor(*items.back()->trace, windows.back(), xStep);
        frame(box, labels, xStep, shared ? &all : nullptr);

        std::vector<std::pair<std::string, Color>> values;
        for (size_t k = 0; k < count; k++)
        {
            const Item& item = *items[k];
            const double w = count > 1 ? 1.0 - static_cast<double>(k) / static_cast<double>(count - 1) : 0.0;
            const Window& window = windows[k];
            const double span = window.last - window.first;
            const float baseline = at(box, 0, 0, w).y;
            double lastValue = 0.0;
            bool haveValue = false;
            const auto runs = traceRuns(
                *item.trace, window,
                [&](double t) { return at(box, span > 0 ? (t - window.first) / span : 1.0, 0, w).x; },
                [&](double value) { return at(box, 0, share(ranges[k], value), w).y; }, lastValue, haveValue);
            for (const auto& run : runs)
                out.fillUnder(run, baseline, mix(item.color, kBackground, 0.72f), 235);
            out.line(at(box, 0, 0, w), at(box, 1, 0, w), 1.0f, mix(item.color, kBackground, 0.4f));
            for (const auto& run : runs)
                out.polyline(run, settings.lineThickness, item.color);
            const Point end = at(box, 1, 0, w);
            out.text(item.caption, end.x + 8.0f, end.y - Recorder::lineHeight() / 2.0f, item.color);
            if (haveValue)
                values.emplace_back(item.caption + " = " + format("%.3f", lastValue), item.color);
        }
        titles(items, laneTop + 8.0f);
        if (settings.showLastValue)
            for (size_t i = 0; i < values.size(); i++)
                out.text(values[i].first, box.right, box.top + 4.0f + static_cast<float>(i) * 20.0f, kAxis, false, Align::Right);
    }

    /// A vector signal, such as a spectrum, in 3D: a waterfall of its values
    /// over the duration, the newest at the front.
    void waterfall(const Box& box, const Item& item, float laneTop)
    {
        const Trace& trace = *item.trace;
        const auto& history = trace.vectorHistory;
        size_t offset;
        size_t count;
        vectorSpan(trace, settings, offset, count);
        std::vector<const std::pair<double, std::vector<double>>*> slices;
        const double last = history.empty() ? 0.0 : history.back().first;
        for (const auto& slice : history)
            if (slice.first >= last - settings.duration)
                slices.push_back(&slice);
        // At most 48 slices, evenly spread, so the waterfall stays readable.
        if (slices.size() > 48)
        {
            std::vector<const std::pair<double, std::vector<double>>*> fewer;
            for (size_t i = 0; i < 48; i++)
                fewer.push_back(slices[(slices.size() - 1) - (47 - i) * (slices.size() - 1) / 47]);
            slices = fewer;
        }

        Range range;
        if (settings.useCustomMinMaxValue)
            range = {settings.customMinValue, settings.customMaxValue};
        else if (trace.hasValueRange)
            range = {trace.rangeLow, trace.rangeHigh};
        else
        {
            range = {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
            for (const auto* slice : slices)
                for (size_t i = offset; i < offset + count && i < slice->second.size(); i++)
                    if (std::isfinite(slice->second[i]))
                    {
                        range.low = std::min(range.low, slice->second[i]);
                        range.high = std::max(range.high, slice->second[i]);
                    }
            if (!(range.low <= range.high))
                range = {0.0, 1.0};
            const double pad = std::max((range.high - range.low) * 0.05, 1e-9);
            range = {range.low - pad, range.high + pad};
        }

        size_t xStep = 1;
        const auto labels = labelsFor(trace, Window{}, xStep);
        frame(box, labels, xStep, &range);

        for (const auto* slice : slices)
        {
            const double w = settings.duration > 0 ? std::clamp((last - slice->first) / settings.duration, 0.0, 1.0) : 0.0;
            std::vector<Point> points;
            for (size_t i = 0; i < count && offset + i < slice->second.size(); i++)
                points.push_back(at(box, count > 1 ? static_cast<double>(i) / (count - 1) : 0.0, share(range, slice->second[offset + i]), w));
            out.fillUnder(points, at(box, 0, 0, w).y, kBackground, 235);
            out.polyline(points, settings.lineThickness, mix(item.color, kBackground, 0.7f * static_cast<float>(w)));
        }

        const float lineHeight = Recorder::lineHeight();
        const Point front = at(box, 1, 0, 0);
        const Point back = at(box, 1, 0, 1);
        out.text("now", front.x + 8.0f, front.y - lineHeight / 2.0f, kAxis);
        out.text(format("-%.2f s", settings.duration), back.x + 8.0f, back.y - lineHeight / 2.0f, kAxis);
        out.text(item.caption, (box.left + box.right) / 2.0f, laneTop + 8.0f, item.color, true, Align::Center);
        if (settings.showLastValue && !trace.vectorValues.empty())
        {
            const std::string text = "Value = " + format("%.3f", trace.vectorValues.front());
            out.text(text, box.right, box.top + 4.0f, kAxis, false, Align::Right);
        }
    }

    void draw3D(const std::vector<Item>& items, Problems& problems)
    {
        std::vector<const Item*> scalars;
        std::vector<Item> scalarItems;
        std::vector<Window> windows;
        std::vector<const Item*> vectors;
        for (const auto& item : items)
        {
            if (item.trace->vector)
            {
                vectors.push_back(&item);
                continue;
            }
            scalars.push_back(&item);
            scalarItems.push_back(item);
            const auto& samples = item.trace->samples;
            const double last = samples.empty() ? 0.0 : item.end;
            windows.push_back({last - settings.duration, last});
        }
        // The scalar signals share their time axis where they can.
        if (scalarItems.size() > 1)
            shareXAxis(scalarItems, windows, problems);

        const size_t lanes = (scalars.empty() ? 0 : 1) + vectors.size();
        const float width = static_cast<float>(out.width());
        const float laneHeight = areaHeight / static_cast<float>(lanes);
        // Room at the right for the planes' names or the depth labels.
        float names = 70.0f;
        for (const auto* item : scalars)
            names = std::max(names, std::min(Recorder::textWidth(item->caption) + 12.0f, width * 0.3f));
        size_t lane = 0;
        const auto boxAt = [&](size_t index)
        {
            const float top = laneHeight * static_cast<float>(index);
            Box box{75.0f, top + kLabelHeight, width - 25.0f - names, top + laneHeight - kLabelHeight, 0.0f, 0.0f};
            box.dx = (box.right - box.left) * 0.22f;
            box.dy = (box.bottom - box.top) * 0.38f;
            return box;
        };
        if (!scalars.empty())
            scalars3D(boxAt(lane), scalars, windows, laneHeight * static_cast<float>(lane)), lane++;
        for (const auto* item : vectors)
            waterfall(boxAt(lane), *item, laneHeight * static_cast<float>(lane)), lane++;
    }

    // ---------------------------------------------------------------- shared

    /// The x labels of a signal: its vector's labels, or times over the
    /// window. `step` receives the tick step.
    std::vector<std::string> labelsFor(const Trace& trace, const Window& window, size_t& step) const
    {
        std::vector<std::string> labels;
        step = 1;
        if (trace.vector)
        {
            size_t offset;
            size_t count;
            vectorSpan(trace, settings, offset, count);
            if (count > 11)
                step = (count + 10) / 11;
            for (size_t i = 0; i < count; i++)
                labels.push_back(offset + i < trace.vectorLabels.size() ? trace.vectorLabels[offset + i] : std::to_string(offset + i));
            return labels;
        }
        constexpr size_t ticks = 5;
        for (size_t i = 0; i < ticks; i++)
        {
            const double part = static_cast<double>(i) / static_cast<double>(ticks - 1);
            if (trace.hasTimeOrigin)
                labels.push_back(i % 2 == 1 ? "" : localTime(window.last - settings.duration * (1.0 - part)));
            else
                labels.push_back(format("%.2f", settings.duration * part));
        }
        return labels;
    }

    /// Draws x labels under their ticks. The last label is always the last
    /// tick's. A label that would run into the one before it is left out, and
    /// before the last label, the one before it gives way.
    template <typename XAt>
    void xLabels(const XAt& xAt, const std::vector<std::string>& labels, size_t step, float top)
    {
        const size_t ticks = labels.size();
        struct Placed
        {
            float left;
            float right;
            float anchor;
            Align align;
            const std::string* text;
        };
        std::vector<Placed> placed;
        for (size_t i = 0; i < ticks; i += step)
        {
            if (ticks - 1 - i < step)
                i = ticks - 1;
            const auto& text = labels[i];
            const float textWidth = Recorder::textWidth(text);
            const Align align = i == 0 ? Align::Left : i == ticks - 1 ? Align::Right : Align::Center;
            const float anchor = xAt(i);
            const float left = anchor - (align == Align::Left ? 0.0f : align == Align::Right ? textWidth : textWidth / 2.0f);
            const bool last = i == ticks - 1;
            while (!placed.empty() && left < placed.back().right + 8.0f)
            {
                if (!last)
                    break;
                placed.pop_back();
            }
            if (placed.empty() || left >= placed.back().right + 8.0f)
                placed.push_back({left, left + textWidth, anchor, align, &text});
        }
        for (const auto& label : placed)
            out.text(*label.text, label.anchor, top, kAxis, false, label.align);
    }

    /// The line of a signal's samples in a window, in runs split at gaps.
    /// Many samples share a pixel column on a long duration; each column
    /// keeps its first, lowest, highest and last sample, which draws the same
    /// line as all of them would.
    template <typename XOf, typename YOf>
    static std::vector<std::vector<Point>> traceRuns(
        const Trace& trace, const Window& window, const XOf& xOf, const YOf& yOfValue, double& lastValue, bool& haveValue)
    {
        std::vector<std::vector<Point>> runs(1);
        const auto begin = std::lower_bound(trace.samples.begin(), trace.samples.end(), window.first,
                                            [](const Sample& sample, double t) { return sample.t < t; });
        int column = std::numeric_limits<int>::min();
        Point firstPoint{};
        Point lastPoint{};
        float low = 0.0f;
        float high = 0.0f;
        size_t lowAt = 0;
        size_t highAt = 0;
        size_t inColumn = 0;
        const auto flush = [&]
        {
            if (inColumn == 0)
                return;
            auto& points = runs.back();
            points.push_back(firstPoint);
            if (inColumn > 2)
            {
                const float x = static_cast<float>(column) + 0.5f;
                const bool lowFirst = lowAt < highAt;
                points.push_back({x, lowFirst ? low : high});
                points.push_back({x, lowFirst ? high : low});
            }
            if (inColumn > 1)
                points.push_back(lastPoint);
            inColumn = 0;
        };
        for (auto it = begin; it != trace.samples.end(); ++it)
        {
            if (!std::isfinite(it->value))
                continue;
            if (it->gapBefore && !runs.back().empty())
            {
                flush();
                runs.emplace_back();
                column = std::numeric_limits<int>::min();
            }
            const Point point{xOf(it->t), yOfValue(it->value)};
            const int at = static_cast<int>(std::floor(point.x));
            if (at != column)
            {
                flush();
                column = at;
                firstPoint = point;
                low = high = point.y;
                lowAt = highAt = 0;
            }
            else
            {
                if (point.y < low)
                {
                    low = point.y;
                    lowAt = inColumn;
                }
                if (point.y > high)
                {
                    high = point.y;
                    highAt = inColumn;
                }
            }
            lastPoint = point;
            inColumn++;
            lastValue = it->value;
            haveValue = true;
        }
        flush();
        return runs;
    }
};

}  // namespace

// ---------------------------------------------------------------- the function block

UniversalRendererFb::UniversalRendererFb(const ModuleInfoPtr& moduleInfo,
                                     const ContextPtr& context,
                                     const ComponentPtr& parent,
                                     const StringPtr& localId)
    : FunctionBlock(CreateType(moduleInfo), context, parent, localId)
{
    // The SDK names a block after its local ID, which it builds from the type
    // ID, such as "UniversalRenderer_1". The tree shows a readable
    // name instead: "UniversalRenderer", then "UniversalRenderer_2" and on.
    const std::string id = localId.toStdString();
    const std::string typeId = "UniversalRenderer";
    if (id.rfind(typeId, 0) == 0)
    {
        const std::string number = id.substr(typeId.size());
        name = "UniversalRenderer" + (number == "_1" ? std::string() : number);
    }

    initComponentStatus();
    initProperties();

    plotTime = createAndAddSignal("PlotTime",
                                     DataDescriptorBuilder()
                                         .setSampleType(SampleType::Int64)
                                         .setTickResolution(Ratio(1, 1'000'000))
                                         .setRule(ExplicitDataRule())
                                         .setOrigin("1970-01-01T00:00:00Z")
                                         .setUnit(Unit("s", -1, "seconds", "time"))
                                         .setName("Time")
                                         .build(),
                                     false);
    int width;
    int height;
    sizeOf(settings.resolution, width, height);
    plot = createAndAddSignal("Plot");
    describePlot(width, height);
    plot.setDomainSignal(plotTime);
    plot.setName("Plot");
    plot.setDescription("The plot, as plot data per sample: its lines, areas, texts and pictures, for openDAQ Studio to draw (unit "
                        "plot; the format is in the module's README). The data descriptor's metadata holds the plot's width and height.");

    pictureTime = createAndAddSignal("PictureTime", plotTime.getDescriptor(), false);
    picture = createAndAddSignal("Picture");
    describePicture(width, height);
    picture.setDomainSignal(pictureTime);
    picture.setName("Picture");
    picture.setDescription("The same plot as one PNG picture per sample, for any client that shows pictures. It is drawn only while "
                           "a client reads it, at most PictureFrameRate a second.");

    updateInputPorts();
    thread = std::thread([this] { run(); });
    pictureThread = std::thread([this] { drawPictures(); });
}

UniversalRendererFb::~UniversalRendererFb()
{
    {
        std::lock_guard guard(webLock);
        web.reset();
    }
    {
        std::lock_guard guard(lock);
        stopping = true;
    }
    wake.notify_all();
    pictureWake.notify_all();
    if (thread.joinable())
        thread.join();
    if (pictureThread.joinable())
        pictureThread.join();
}

FunctionBlockTypePtr UniversalRendererFb::CreateType(const ModuleInfoPtr& moduleInfo)
{
    auto type = FunctionBlockType("UniversalRenderer",
                                  "Universal renderer",
                                  "Signal visualization in 2D or 3D, pictures such as a camera's included, with the properties of "
                                  "the reference renderer: plot data for openDAQ Studio and pictures for any client, instead of a window");
    if (moduleInfo.assigned())
        checkErrorInfo(type.asPtr<IComponentTypePrivate>(true)->setModuleInfo(moduleInfo));
    return type;
}

void UniversalRendererFb::removed()
{
    {
        std::lock_guard guard(webLock);
        web.reset();
        webOn = false;
    }
    {
        std::lock_guard guard(lock);
        stopping = true;
    }
    wake.notify_all();
    pictureWake.notify_all();
    FunctionBlock::removed();
}

void UniversalRendererFb::initProperties()
{
    const auto changed = [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { readProperties(); };
    const auto add = [&](const PropertyPtr& property)
    {
        objPtr.addProperty(property);
        objPtr.getOnPropertyValueWrite(property.getName()) += changed;
    };

    add(FloatPropertyBuilder("Duration", 1.0)
            .setSuggestedValues(List<Float>(0.01, 0.1, 1.0, 10.0))
            .setUnit(Unit("s"))
            .setDescription("The time the plot shows, in seconds.")
            .build());
    add(BoolPropertyBuilder("SingleXAxis", False).setDescription("Puts all signals on one time axis.").build());
    add(BoolPropertyBuilder("SingleYAxis", False)
            .setVisible(EvalValue("$SingleXAxis"))
            .setDescription("Draws all signals in one chart, on one value axis.")
            .build());
    add(FloatPropertyBuilder("LineThickness", 1.0).setDescription("The width of the lines, in pixels.").build());
    add(BoolPropertyBuilder("Freeze", False).setDescription("Holds the plot while data keeps arriving.").build());
    add(SelectionPropertyBuilder("Resolution", List<IString>("640x480", "800x600", "1024x768", "1280x720", "1920x1080", "2560x1440"), 1)
            .setDescription("The size of the plot, which also sets its shape. The viewer scales it to its window.")
            .build());
    add(BoolPropertyBuilder("ShowLastValue", False).setDescription("Shows each signal's newest value.").build());
    add(BoolPropertyBuilder("UseCustomMinMaxValue", False)
            .setDescription("Uses CustomMinValue and CustomMaxValue as the value axis instead of the signal's range.")
            .build());
    add(FloatPropertyBuilder("CustomMaxValue", 10.0).setVisible(EvalValue("$UseCustomMinMaxValue")).build());
    add(FloatPropertyBuilder("CustomMinValue", -10.0).setVisible(EvalValue("$UseCustomMinMaxValue")).build());
    add(BoolPropertyBuilder("UseCustom2dRangeValue", False)
            .setDescription("Draws only the elements from Custom2dMinRange to Custom2dMaxRange of a vector signal, such as a spectrum.")
            .build());
    add(IntPropertyBuilder("Custom2dMinRange", 0).setVisible(EvalValue("$UseCustom2dRangeValue")).build());
    add(IntPropertyBuilder("Custom2dMaxRange", 100).setVisible(EvalValue("$UseCustom2dRangeValue")).build());
    add(SelectionPropertyBuilder("View", List<IString>("2D", "3D"), 0)
            .setDescription("2D draws each signal on its own axes, as the reference renderer. 3D draws the signals on planes "
                            "one behind the other, and a vector signal such as a spectrum as a waterfall over the duration.")
            .build());
    add(IntPropertyBuilder("FrameRate", 25)
            .setMinValue(1)
            .setMaxValue(240)
            .setUnit(Unit("fps"))
            .setDescription("The most plots a second. A plot is sent only when something changed.")
            .build());
    add(IntPropertyBuilder("PictureFrameRate", 10)
            .setMinValue(1)
            .setMaxValue(240)
            .setUnit(Unit("fps"))
            .setDescription("The most pictures a second on the Picture output, at most one per plot. Pictures are made on a "
                            "thread of their own and never slow the plot down.")
            .build());

    // The reference renderer's button. The block has no screen, so the
    // window is the client's: openDAQ Studio opens the Plot signal in a
    // window of its own when this is run from it.
    objPtr.addProperty(FunctionPropertyBuilder("OpenWindow", ProcedureInfo())
                           .setReadOnly(true)
                           .setDescription("Opens the plot in a window of its own, in the openDAQ Studio that runs it.")
                           .build());
    objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("OpenWindow", Procedure([] {}));

    // A small web server, so any browser shows the plot.
    const auto webChanged = [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { applyWeb(); };
    objPtr.addProperty(BoolPropertyBuilder("WebServer", False)
                           .setDescription("Serves the plot to any web browser on the network, at the addresses in WebAddress. "
                                           "Anyone on the network can watch while it runs.")
                           .build());
    objPtr.getOnPropertyValueWrite("WebServer") += webChanged;
    objPtr.addProperty(IntPropertyBuilder("WebPort", 8080)
                           .setMinValue(1024)
                           .setMaxValue(65535)
                           .setDescription("The port the web server listens on.")
                           .build());
    objPtr.getOnPropertyValueWrite("WebPort") += webChanged;
    objPtr.addProperty(StringPropertyBuilder("WebAddress", "")
                           .setReadOnly(true)
                           .setVisible(EvalValue("$WebServer"))
                           .setDescription("Where to open the plot in a browser while the web server runs: / draws the plot, "
                                           "/stream shows it as pictures, /picture is the newest picture and /plot the plot data.")
                           .build());

    readProperties();
}

void UniversalRendererFb::applyWeb()
{
    const bool enabled = objPtr.getPropertyValue("WebServer");
    const int port = static_cast<int>(static_cast<Int>(objPtr.getPropertyValue("WebPort")));
    std::string address;
    std::string error;
    {
        std::lock_guard guard(webLock);
        if (!web)
            web = std::make_unique<WebServer>(webFrames, "Universal renderer");
        web->stop();
        webOn = false;
        if (enabled)
        {
            error = web->start(port);
            webOn = error.empty();
            if (webOn)
                for (const auto& link : WebServer::addresses(port))
                    address += (address.empty() ? "" : "  ") + link;
            else
                address = error;
        }
    }
    {
        std::lock_guard guard(lock);
        webError = error;
        settingsVersion++;
    }
    objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("WebAddress", String(address));
}

void UniversalRendererFb::readProperties()
{
    Settings next;
    next.duration = objPtr.getPropertyValue("Duration");
    next.singleXAxis = objPtr.getPropertyValue("SingleXAxis");
    next.singleYAxis = objPtr.getPropertyValue("SingleYAxis");
    next.lineThickness = static_cast<float>(static_cast<double>(objPtr.getPropertyValue("LineThickness")));
    next.freeze = objPtr.getPropertyValue("Freeze");
    next.resolution = static_cast<int>(static_cast<Int>(objPtr.getPropertyValue("Resolution")));
    next.showLastValue = objPtr.getPropertyValue("ShowLastValue");
    next.useCustomMinMaxValue = objPtr.getPropertyValue("UseCustomMinMaxValue");
    next.customMinValue = objPtr.getPropertyValue("CustomMinValue");
    next.customMaxValue = objPtr.getPropertyValue("CustomMaxValue");
    next.useCustom2dRangeValue = objPtr.getPropertyValue("UseCustom2dRangeValue");
    next.custom2dMinRange = objPtr.getPropertyValue("Custom2dMinRange");
    next.custom2dMaxRange = objPtr.getPropertyValue("Custom2dMaxRange");
    next.frameRate = static_cast<int>(std::clamp<Int>(objPtr.getPropertyValue("FrameRate"), 1, 240));
    next.view3D = static_cast<Int>(objPtr.getPropertyValue("View")) == 1;
    next.pictureFrameRate = static_cast<int>(std::clamp<Int>(objPtr.getPropertyValue("PictureFrameRate"), 1, 240));
    next.duration = std::max(next.duration, 1e-6);
    next.lineThickness = std::clamp(next.lineThickness, 0.5f, 50.0f);
    {
        std::lock_guard guard(lock);
        settings = next;
        settingsVersion++;
    }
    wake.notify_all();
}

// As the reference renderer: one free input port at the end, and the ports
// whose signal was disconnected removed.
void UniversalRendererFb::updateInputPorts()
{
    std::lock_guard guard(lock);
    for (auto it = inputs.begin(); it != inputs.end();)
    {
        if (!(*it)->port.getSignal().assigned())
        {
            removeInputPort((*it)->port);
            it = inputs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    auto input = std::make_shared<Input>();
    input->port = createAndAddInputPort("Input" + std::to_string(portCount++), PacketReadyNotification::None);
    inputs.push_back(std::move(input));
    inputsVersion++;
}

void UniversalRendererFb::onConnected(const InputPortPtr& /*port*/)
{
    auto configLock = getRecursiveConfigLock();
    updateInputPorts();
}

void UniversalRendererFb::onDisconnected(const InputPortPtr& /*port*/)
{
    auto configLock = getRecursiveConfigLock();
    updateInputPorts();
}

void UniversalRendererFb::describePlot(int width, int height)
{
    if (width == plotWidth && height == plotHeight)
        return;
    plotWidth = width;
    plotHeight = height;
    plot.setDescriptor(DataDescriptorBuilder()
                           .setSampleType(SampleType::Binary)
                           .setUnit(Unit("plot"))
                           .setName("Plot")
                           .setMetadata(Dict<IString, IString>({{"width", std::to_string(width)}, {"height", std::to_string(height)}}))
                           .build());
}

void UniversalRendererFb::describePicture(int width, int height)
{
    if (width == pictureWidth && height == pictureHeight)
        return;
    pictureWidth = width;
    pictureHeight = height;
    picture.setDescriptor(DataDescriptorBuilder()
                              .setSampleType(SampleType::Binary)
                              .setUnit(Unit("png"))
                              .setName("Picture")
                              .setMetadata(Dict<IString, IString>({{"width", std::to_string(width)}, {"height", std::to_string(height)}}))
                              .build());
}

/// Sends one sample, stamped now.
void UniversalRendererFb::send(const SignalConfigPtr& signal, const SignalConfigPtr& time, const std::vector<uint8_t>& bytes)
{
    auto domainPacket = DataPacket(time.getDescriptor(), 1);
    *static_cast<int64_t*>(domainPacket.getRawData()) = unixMicroseconds();
    auto packet = BinaryDataPacket(domainPacket, signal.getDescriptor(), bytes.size());
    std::memcpy(packet.getRawData(), bytes.data(), bytes.size());
    time.sendPacket(domainPacket);
    signal.sendPacket(packet);
}

// ---------------------------------------------------------------- the render thread

void UniversalRendererFb::run()
{
    auto next = std::chrono::steady_clock::now();
    while (true)
    {
        std::chrono::microseconds period;
        {
            std::unique_lock guard(lock);
            wake.wait_until(guard, next, [this] { return stopping; });
            if (stopping)
                return;
            period = std::chrono::microseconds(1'000'000 / settings.frameRate);
        }
        next += period;
        // A late frame is followed by the next one at once, but not by a
        // burst of all the frames it missed.
        const auto now = std::chrono::steady_clock::now();
        if (next < now)
            next = now;
        try
        {
            frame();
        }
        catch (const std::exception& error)
        {
            report(ComponentStatus::Error, std::string("Drawing failed: ") + error.what());
        }
    }
}

void UniversalRendererFb::frame()
{
    Settings now;
    std::vector<std::shared_ptr<Input>> current;
    int seenSettings;
    int seenInputs;
    {
        std::lock_guard guard(lock);
        now = settings;
        current = inputs;
        seenSettings = settingsVersion;
        seenInputs = inputsVersion;
    }

    bool fresh = false;
    std::vector<Input*> connected;
    for (auto& input : current)
    {
        fresh = drain(*input, now) || fresh;
        if (input->port.getSignal().assigned())
            connected.push_back(input.get());
    }

    // Freezing holds what is on screen; the inputs keep reading, so the plot
    // continues with current data when it is released.
    if (now.freeze && (!isFrozen || seenInputs != drawnInputsVersion))
    {
        frozen.clear();
        for (auto* input : connected)
            frozen.push_back(input->trace);
        isFrozen = true;
    }
    else if (!now.freeze && isFrozen)
    {
        frozen.clear();
        isFrozen = false;
        fresh = true;
    }

    // While data flows on a clock time axis, the axis moves with the clock,
    // so every frame differs, and the plot scrolls at the frame rate instead
    // of jumping by the device's packets.
    const auto clock = std::chrono::steady_clock::now();
    const double wall = unixMicroseconds() / 1e6;
    const auto followsClock = [&](const Trace& trace)
    { return !isFrozen && trace.hasTimeOrigin && std::isfinite(trace.clockLag) && clock - trace.arrived < kHeartbeat; };
    bool scrolling = false;
    for (auto* input : connected)
        scrolling = scrolling || (input->trace.valid && !input->trace.vector && followsClock(input->trace));
    const bool due = (fresh && !isFrozen) || scrolling || seenSettings != drawnSettingsVersion || seenInputs != drawnInputsVersion ||
                     clock - lastFrame >= kHeartbeat;
    // The Picture output is drawn only while a client reads it: a client's
    // reader, or the streaming server for a remote one, connects to it.
    const bool watched = picture.getConnections().getCount() > 0 || (webOn && webFrames->picturesWanted());
    // Pictures come on plot frames, so one is due half a plot frame early
    // rather than a whole one late.
    const auto picturePeriod = std::chrono::microseconds(1'000'000 / now.pictureFrameRate - 500'000 / now.frameRate);
    const bool pictureDue =
        watched && (pictureStale || due || clock - lastPicture >= kHeartbeat) && clock - lastPicture >= picturePeriod;
    if (!due && !pictureDue)
        return;

    Problems problems;
    {
        std::lock_guard guard(lock);
        if (!webError.empty())
            problems.add(ComponentStatus::Warning, webError);
        if (!pictureError.empty())
            problems.add(ComponentStatus::Warning, pictureError);
    }
    if (now.custom2dMinRange > now.custom2dMaxRange)
        problems.add(ComponentStatus::Error, "Property Custom2dMaxRange has to be more than Custom2dMinRange");
    std::vector<Item> items;
    for (size_t i = 0; i < connected.size(); i++)
    {
        const Trace& trace = isFrozen && i < frozen.size() ? frozen[i] : connected[i]->trace;
        if (!trace.valid)
            continue;
        std::string caption = "N/A";
        if (const auto signal = connected[i]->port.getSignal(); signal.assigned())
            caption = signal.getName().toStdString();
        if (!trace.unit.empty())
            caption += " [" + trace.unit + "]";
        double end = trace.samples.empty() ? 0.0 : trace.samples.back().t;
        if (!trace.vector && !trace.image && followsClock(trace))
            end = std::max(end, wall - trace.clockLag);
        items.push_back({&trace, caption, colorOf(i), i, end});
    }
    for (auto* input : connected)
        if (!input->trace.valid && input->valueDescriptor.assigned())
            problems.add(ComponentStatus::Warning, input->port.getName().toStdString() + ": the signal cannot be drawn");

    // For a picture, the plot is laid out once on a tape, played into plot
    // data here and handed to the picture thread.
    int width;
    int height;
    sizeOf(now.resolution, width, height);
    std::unique_ptr<Tape> tape;
    if (pictureDue)
    {
        tape = std::make_unique<Tape>(width, height);
        Plot(*tape, now).draw(items, problems);
    }
    if (due)
    {
        drawnSettingsVersion = seenSettings;
        drawnInputsVersion = seenInputs;
        lastFrame = clock;
        Recorder recorder(width, height);
        if (tape)
            tape->play(recorder);
        else
            Plot(recorder, now).draw(items, problems);
        describePlot(width, height);
        auto bytes = recorder.bytes();
        send(plot, plotTime, bytes);
        if (webOn)
            webFrames->publishPlot(std::move(bytes));
        pictureStale = true;
    }

    if (pictureDue)
    {
        {
            std::lock_guard guard(lock);
            pendingPicture = std::move(tape);
        }
        pictureWake.notify_one();
        lastPicture = clock;
        pictureStale = false;
    }

    if (due)
        report(problems.status, problems.message);
}

// ---------------------------------------------------------------- the picture thread

void UniversalRendererFb::drawPictures()
{
    std::unique_ptr<Canvas> canvas;
    // Pictures from the inputs, decoded once each, by their frame's number.
    std::map<uint32_t, std::shared_ptr<const Frame>> decoded;
    while (true)
    {
        std::unique_ptr<Tape> tape;
        {
            std::unique_lock guard(lock);
            pictureWake.wait(guard, [this] { return stopping || pendingPicture; });
            if (stopping)
                return;
            tape = std::move(pendingPicture);
        }
        std::string error;
        try
        {
            const int width = tape->width();
            const int height = tape->height();
            if (!canvas || canvas->width() != width || canvas->height() != height)
                canvas = std::make_unique<Canvas>(width, height);
            std::map<uint32_t, std::shared_ptr<const Frame>> used;
            tape->play(*canvas,
                       [&](const std::shared_ptr<const Frame>& frame)
                       {
                           auto& out = used[frame->id];
                           if (const auto found = decoded.find(frame->id); found != decoded.end())
                               out = found->second;
                           if (out)
                               return out;
                           auto pixels = std::make_shared<Frame>();
                           pixels->id = frame->id;
                           pixels->format = frame->format;
                           int w = 0;
                           int h = 0;
                           if (frame->format == "jpeg" && decodeJpeg(frame->bytes, w, h, pixels->rgb))
                           {
                               pixels->width = w;
                               pixels->height = h;
                           }
                           else
                           {
                               pixels->rgb.clear();
                               pixels->decodeFailed = true;
                           }
                           out = pixels;
                           return out;
                       });
            decoded = std::move(used);
            auto png = encodePng(canvas->pixels(), width, height);
            describePicture(width, height);
            send(picture, pictureTime, png);
            if (webOn)
                webFrames->publishPicture(std::move(png));
        }
        catch (const std::exception& failure)
        {
            error = std::string("Drawing the picture failed: ") + failure.what();
        }
        std::lock_guard guard(lock);
        pictureError = error;
    }
}

/// Reads everything that arrived on an input. Returns whether anything did.
bool UniversalRendererFb::drain(Input& input, const Settings& now)
{
    const auto connection = input.port.getConnection();
    if (!connection.assigned())
        return false;
    bool fresh = false;
    for (PacketPtr packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        fresh = true;
        if (packet.getType() == PacketType::Event)
        {
            const auto event = packet.asPtr<IEventPacket>(true);
            if (event.getEventId() != event_packet_id::DATA_DESCRIPTOR_CHANGED)
                continue;
            const DataDescriptorPtr value = event.getParameters().get(event_packet_param::DATA_DESCRIPTOR);
            const DataDescriptorPtr domain = event.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR);
            if (value.assigned())
                input.valueDescriptor = value;
            if (domain.assigned())
                input.domainDescriptor = domain;
            configure(input);
        }
        else if (packet.getType() == PacketType::Data)
        {
            addData(input.trace, packet.asPtr<IDataPacket>(true), now);
        }
    }
    return fresh;
}

void UniversalRendererFb::configure(Input& input)
{
    Trace& trace = input.trace;
    trace = Trace{};
    const auto& value = input.valueDescriptor;
    const auto& domain = input.domainDescriptor;
    if (!value.assigned())
        return;
    // A picture signal, such as a camera's: Binary samples whose unit names
    // the format, openDAQ's video convention.
    if (value.getSampleType() == SampleType::Binary)
    {
        std::string format = value.getUnit().assigned() ? value.getUnit().getSymbol().toStdString() : "";
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (format == "jpg")
            format = "jpeg";
        if (format != "jpeg" && format != "png")
            return;
        trace.image = true;
        trace.imageFormat = format;
        trace.valid = true;
        return;
    }
    if (!domain.assigned())
        return;
    if (value.getSampleType() == SampleType::Null || domain.getSampleType() == SampleType::Null)
        return;
    if (!isNumber(valueTypeOf(value)))
        return;
    const auto dimensions = value.getDimensions();
    const size_t dimensionCount = dimensions.assigned() ? dimensions.getCount() : 0;
    if (dimensionCount > 1)
        return;
    if (!domain.getTickResolution().assigned())
        return;

    if (dimensionCount == 1)
    {
        trace.vector = true;
        const auto labels = dimensions[0].getLabels();
        for (const auto& label : labels)
        {
            if (label.supportsInterface(IFloat::Id))
                trace.vectorLabels.push_back(format("%.2f", static_cast<Float>(label)));
            else if (label.supportsInterface(IInteger::Id))
                trace.vectorLabels.push_back(std::to_string(static_cast<Int>(label)));
            else
                trace.vectorLabels.push_back(label.toString());
        }
    }

    const auto resolution = domain.getTickResolution();
    trace.tick = static_cast<double>(resolution.getNumerator()) / static_cast<double>(resolution.getDenominator());
    const auto domainUnit = domain.getUnit();
    if (domainUnit.assigned())
    {
        trace.domainUnit = domainUnit.getSymbol().toStdString();
        trace.domainQuantity = domainUnit.getQuantity().toStdString();
    }
    const auto origin = domain.getOrigin();
    if (origin.assigned() && !origin.toStdString().empty() && trace.domainUnit == "s" && trace.domainQuantity == "time")
    {
        const auto epoch = reader::parseEpoch(origin.toStdString());
        trace.origin = std::chrono::duration<double>(epoch.time_since_epoch()).count();
        trace.hasTimeOrigin = true;
    }

    const auto range = value.getValueRange();
    if (range.assigned())
    {
        trace.hasValueRange = true;
        trace.rangeLow = range.getLowValue();
        trace.rangeHigh = range.getHighValue();
    }
    const auto unit = value.getUnit();
    if (unit.assigned())
        trace.unit = unit.getSymbol().toStdString();
    trace.valid = true;
}

void UniversalRendererFb::addData(Trace& trace, const DataPacketPtr& packet, const Settings& now)
{
    if (!trace.valid)
        return;
    if (trace.image)
    {
        // The newest picture replaces the last; a frozen plot keeps its own.
        const auto* raw = static_cast<const uint8_t*>(packet.getRawData());
        const size_t size = packet.getRawDataSize();
        if (!raw || size == 0)
            return;
        auto frame = std::make_shared<Frame>();
        frame->id = ++frameCount;
        frame->format = trace.imageFormat;
        frame->bytes.assign(raw, raw + size);
        const auto metadata = packet.getDataDescriptor().getMetadata();
        if (metadata.assigned() && metadata.hasKey("width") && metadata.hasKey("height"))
        {
            frame->width = std::atoi(metadata.get("width").toStdString().c_str());
            frame->height = std::atoi(metadata.get("height").toStdString().c_str());
        }
        trace.frame = std::move(frame);
        return;
    }
    const auto domainPacket = packet.getDomainPacket();
    const size_t count = packet.getSampleCount();
    if (!domainPacket.assigned() || count == 0)
        return;
    const auto descriptor = packet.getDataDescriptor();
    const SampleType type = valueTypeOf(descriptor);
    const void* data = packet.getData();
    if (!data)
        return;

    const auto domainDescriptor = domainPacket.getDataDescriptor();
    int64_t reference = 0;
    if (const auto info = domainDescriptor.getReferenceDomainInfo(); info.assigned())
        if (const auto offset = info.getReferenceDomainOffset(); offset.assigned())
            reference = offset;
    const auto rule = domainDescriptor.getRule();
    bool gap = false;
    std::vector<double> ticks(count);
    if (rule.getType() == DataRuleType::Linear)
    {
        const auto parameters = rule.getParameters();
        const int64_t delta = parameters.get("delta");
        const int64_t start = parameters.get("start");
        const auto offset = domainPacket.getOffset();
        const int64_t first = reference + start + (offset.assigned() ? offset.getIntValue() : 0);
        gap = trace.expectsTick && first != trace.nextTick;
        trace.nextTick = first + delta * static_cast<int64_t>(count);
        trace.expectsTick = true;
        for (size_t i = 0; i < count; i++)
            ticks[i] = static_cast<double>(first + delta * static_cast<int64_t>(i));
    }
    else
    {
        const void* domainData = domainPacket.getData();
        const SampleType domainType = domainDescriptor.getSampleType();
        if (!domainData || !isNumber(domainType))
            return;
        for (size_t i = 0; i < count; i++)
            ticks[i] = read(domainData, domainType, i) + static_cast<double>(reference);
    }

    if (trace.vector)
    {
        // The newest vector is drawn in 2D; in 3D, the ones of the duration.
        const size_t size = descriptor.getDimensions()[0].getSize();
        for (size_t sample = 0; sample < count; sample++)
        {
            std::vector<double> values(size);
            for (size_t i = 0; i < size; i++)
                values[i] = read(data, type, sample * size + i);
            trace.vectorHistory.emplace_back(trace.origin + ticks[sample] * trace.tick, std::move(values));
        }
        trace.vectorValues = trace.vectorHistory.back().second;
        const double keep = trace.vectorHistory.back().first - now.duration * 1.05;
        while (!trace.vectorHistory.empty() && (trace.vectorHistory.front().first < keep || trace.vectorHistory.size() > 600))
            trace.vectorHistory.pop_front();
        return;
    }

    for (size_t i = 0; i < count; i++)
        trace.samples.push_back({trace.origin + ticks[i] * trace.tick, read(data, type, i), gap && i == 0});

    // The usual delay is the smallest seen lately: it drops at once to a
    // smaller one and creeps up slowly, so a late packet does not shift the
    // axis.
    if (trace.hasTimeOrigin)
    {
        const double lag = unixMicroseconds() / 1e6 - trace.samples.back().t;
        trace.clockLag = !std::isfinite(trace.clockLag) || lag < trace.clockLag ? lag : trace.clockLag + (lag - trace.clockLag) * 0.02;
        trace.arrived = std::chrono::steady_clock::now();
    }

    // Keep what the plot can show, and a little more.
    const double keep = trace.samples.back().t - now.duration * 1.05;
    while (!trace.samples.empty() && (trace.samples.front().t < keep || trace.samples.size() > kMaxSamples))
        trace.samples.pop_front();
}

void UniversalRendererFb::report(ComponentStatus status, const std::string& message)
{
    if (status == shownStatus && message == shownMessage)
        return;
    shownStatus = status;
    shownMessage = message;
    if (status == ComponentStatus::Ok)
        setComponentStatus(ComponentStatus::Ok);
    else
        setComponentStatusWithMessage(status, message);
}

}  // namespace daq::modules::universal_renderer
