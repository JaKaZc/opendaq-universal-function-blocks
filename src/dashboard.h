// The Dashboard function block (UniversalDashboard): a web page of live
// panels, one per connected signal, served by the device that runs it, so any
// browser on the network watches the signals without openDAQ Studio.
#pragma once

#include "signal_reader.h"
#include "web.h"

#include <opendaq/function_block_impl.h>
#include <opendaq/opendaq.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace daq::modules::universal_renderer
{

class DashboardFb final : public FunctionBlock
{
public:
    DashboardFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId);
    ~DashboardFb() override;

    static FunctionBlockTypePtr CreateType(const ModuleInfoPtr& moduleInfo);

    void onConnected(const InputPortPtr& port) override;
    void onDisconnected(const InputPortPtr& port) override;

protected:
    void removed() override;

private:
    /// One connected signal: what it is and what the page shows of it.
    struct Panel
    {
        InputPortConfigPtr port;
        SignalReader reader;
        std::string signalId;
        std::string name;
        std::string owner;
        std::deque<std::pair<double, double>> points;  // time in seconds, value
        double last = 0;
        bool hasLast = false;
        std::vector<double> vectorValues;
        std::vector<uint8_t> picture;
        uint64_t pictureId = 0;
    };

    void updateInputPorts();
    void run();
    bool drain(Panel& panel);
    std::string json();
    bool answer(const std::string& method, const std::string& target, const std::string& query, const std::string& body, WebResponse& response);
    /// The settings routes under /api: the device's components and their
    /// properties, and changes to them, which need the PIN.
    bool settings(const std::string& method, const std::string& target, const std::string& query, const std::string& body, WebResponse& response);
    /// Whether a change may go ahead: the form's PIN is right. Answers the
    /// refusal otherwise.
    bool permitted(const std::string& form, WebResponse& response);
    /// The device the Dashboard belongs to.
    DevicePtr owner();
    void applyWeb();

    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Panel>> panels;
    size_t portCount = 0;
    uint64_t sequence = 0;
    std::string title = "Dashboard";
    std::string pin;
    int failures = 0;
    std::chrono::steady_clock::time_point lockedUntil;
    std::atomic<double> duration{10.0};

    std::shared_ptr<WebFrames> frames;
    std::unique_ptr<WebServer> server;
    std::atomic<bool> stopping{false};
    std::thread worker;
};

}  // namespace daq::modules::universal_renderer
