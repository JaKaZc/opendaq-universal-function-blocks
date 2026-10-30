// The Universal renderer function block (UniversalRenderer): the
// reference renderer's plot, with pictures such as a camera's, sent as plot
// data for openDAQ Studio to draw and as pictures for any other client,
// instead of drawn in a window.
#pragma once

#include "canvas.h"
#include "recorder.h"
#include "tape.h"
#include "web.h"

#include <opendaq/opendaq.h>
#include <opendaq/function_block_impl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace daq::modules::universal_renderer
{

/// The properties, as the reference renderer (RefFBModuleRenderer) has them.
struct Settings
{
    double duration = 1.0;
    bool singleXAxis = false;
    bool singleYAxis = false;
    float lineThickness = 1.0f;
    bool freeze = false;
    int resolution = 1;
    bool showLastValue = false;
    bool useCustomMinMaxValue = false;
    double customMinValue = -10.0;
    double customMaxValue = 10.0;
    bool useCustom2dRangeValue = false;
    int64_t custom2dMinRange = 0;
    int64_t custom2dMaxRange = 100;
    int frameRate = 25;
    int pictureFrameRate = 10;
    bool view3D = false;
};

struct Sample
{
    double t;
    double value;
    bool gapBefore;
};

/// What the renderer knows of one input signal.
struct Trace
{
    bool valid = false;
    // A picture signal, such as a camera's: its newest picture.
    bool image = false;
    std::string imageFormat;
    std::shared_ptr<Frame> frame;
    bool vector = false;
    std::vector<std::string> vectorLabels;
    std::vector<double> vectorValues;
    // Earlier values of a vector signal, with their times, for the 3D view.
    std::deque<std::pair<double, std::vector<double>>> vectorHistory;

    // A sample's time is origin + ticks * tick, in seconds since the Unix
    // epoch when the domain has a time origin, otherwise in domain units.
    bool hasTimeOrigin = false;
    double origin = 0.0;
    double tick = 1.0;
    std::string domainUnit;
    std::string domainQuantity;

    bool hasValueRange = false;
    double rangeLow = 0.0;
    double rangeHigh = 1.0;
    std::string unit;

    std::deque<Sample> samples;
    // How far the newest sample lags the computer's clock, in seconds, and
    // when the last data arrived, so the time axis can follow the clock
    // between packets. Only for a domain with a time origin.
    double clockLag = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point arrived;
    bool expectsTick = false;
    int64_t nextTick = 0;
};

struct Input
{
    InputPortConfigPtr port;
    DataDescriptorPtr valueDescriptor;
    DataDescriptorPtr domainDescriptor;
    Trace trace;
};

class UniversalRendererFb final : public FunctionBlock
{
public:
    UniversalRendererFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId);
    ~UniversalRendererFb() override;

    static FunctionBlockTypePtr CreateType(const ModuleInfoPtr& moduleInfo);

    void onConnected(const InputPortPtr& port) override;
    void onDisconnected(const InputPortPtr& port) override;

protected:
    void removed() override;

private:
    // The plot, as plot data per sample, and its time.
    SignalConfigPtr plot;
    SignalConfigPtr plotTime;
    // The same plot as a PNG picture per sample, made only while a client
    // reads it, and its time.
    SignalConfigPtr picture;
    SignalConfigPtr pictureTime;

    std::mutex lock;
    Settings settings;
    int settingsVersion = 0;
    std::vector<std::shared_ptr<Input>> inputs;
    int inputsVersion = 0;
    int portCount = 0;

    std::thread thread;
    std::condition_variable wake;
    bool stopping = false;

    // The picture thread draws the newest plot handed to it into pixels,
    // encodes and sends it, so the plot's frames never wait for a picture.
    // A plot handed over while it works replaces the one waiting.
    std::thread pictureThread;
    std::condition_variable pictureWake;
    std::unique_ptr<Tape> pendingPicture;
    std::string pictureError;

    // The web server, run and stopped from its properties, and what it
    // serves; its error, if it could not start.
    std::mutex webLock;
    std::unique_ptr<WebServer> web;
    std::shared_ptr<WebFrames> webFrames = std::make_shared<WebFrames>();
    std::atomic<bool> webOn{false};
    std::string webError;

    // Owned by the render thread.
    std::vector<Trace> frozen;
    bool isFrozen = false;
    int drawnSettingsVersion = -1;
    int drawnInputsVersion = -1;
    std::chrono::steady_clock::time_point lastFrame;
    int plotWidth = 0;
    int plotHeight = 0;
    std::chrono::steady_clock::time_point lastPicture;
    // Whether the plot changed since the last picture.
    bool pictureStale = true;
    uint32_t frameCount = 0;
    ComponentStatus shownStatus = ComponentStatus::Ok;
    std::string shownMessage;

    // Owned by the picture thread.
    int pictureWidth = 0;
    int pictureHeight = 0;

    void initProperties();
    void applyWeb();
    void readProperties();
    void updateInputPorts();
    void describePlot(int width, int height);
    void describePicture(int width, int height);
    void send(const SignalConfigPtr& signal, const SignalConfigPtr& time, const std::vector<uint8_t>& bytes);

    void run();
    void frame();
    void drawPictures();
    bool drain(Input& input, const Settings& now);
    void configure(Input& input);
    void addData(Trace& trace, const DataPacketPtr& packet, const Settings& now);
    void report(ComponentStatus status, const std::string& message);
};

}  // namespace daq::modules::universal_renderer
