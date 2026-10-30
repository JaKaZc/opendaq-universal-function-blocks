// A very small web server that shows the renderer's plot in any browser.
//
// Plain sockets and HTTP/1.1, no library. It serves:
//
//   /          a page that draws the plot data in the browser (JavaScript)
//   /plot      the newest plot data; ?after=N waits up to a second for a newer one
//   /picture   the newest picture of the plot, PNG
//   /stream    the pictures as a live stream (multipart/x-mixed-replace), which
//              browsers show without JavaScript and video tools can open
//
// Anyone on the network can watch while it runs; it changes nothing.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace daq::modules::universal_renderer
{

/// The newest plot and picture, shared by the renderer and the server.
class WebFrames
{
public:
    void publishPlot(std::vector<uint8_t> bytes);
    void publishPicture(std::vector<uint8_t> bytes);

    /// Waits up to `timeout` for a plot newer than `after`. Returns false if
    /// none came.
    bool waitPlot(uint64_t after, std::chrono::milliseconds timeout, std::vector<uint8_t>& bytes, uint64_t& sequence);
    bool waitPicture(uint64_t after, std::chrono::milliseconds timeout, std::vector<uint8_t>& bytes, uint64_t& sequence);

    /// Whether a web client wants pictures: one asked in the last two
    /// seconds, or a stream is open.
    bool picturesWanted();
    void wantPictures();
    void streamOpened();
    void streamClosed();

    /// Wakes every waiting client, for the server to stop, and lets clients
    /// wait again when it starts.
    void release();
    void renew();

private:
    std::mutex lock;
    std::condition_variable changed;
    std::vector<uint8_t> plot;
    uint64_t plotSequence = 0;
    std::vector<uint8_t> picture;
    uint64_t pictureSequence = 0;
    std::chrono::steady_clock::time_point pictureAsked;
    int streams = 0;
    bool released = false;
};

/// An answer of a route the owner of a server adds.
struct WebResponse
{
    int status = 200;
    std::string type = "text/plain";
    std::string body;
};

/// Answers a GET or POST of `target` (the path) with `query` and, for a POST,
/// its form-encoded `body`, or returns false to leave it to the server's own
/// routes. It may wait, on the request's thread.
using WebRoute = std::function<bool(const std::string& method,
                                    const std::string& target,
                                    const std::string& query,
                                    const std::string& body,
                                    WebResponse& response)>;

/// A field of a query or of a form-encoded body, decoded, or empty.
std::string formField(const std::string& form, const std::string& name);

class WebServer
{
public:
    explicit WebServer(std::shared_ptr<WebFrames> frames, std::string title);
    ~WebServer();

    /// Starts listening on all interfaces. Returns an empty string, or why it
    /// could not.
    std::string start(int port);
    void stop();
    bool running() const { return listener >= 0; }

    /// The addresses to open in a browser, such as http://192.168.1.39:8080/.
    static std::vector<std::string> addresses(int port);

    /// Routes answered before the server's own, such as a dashboard's page.
    void setRoute(WebRoute value) { route = std::move(value); }

private:
    std::shared_ptr<WebFrames> frames;
    std::string title;
    WebRoute route;
    int listener = -1;
    std::thread acceptor;
    std::atomic<bool> stopping{false};

    std::mutex clientsLock;
    std::condition_variable clientsDone;
    std::set<int> clients;

    void acceptLoop();
    void serve(int socket);
};

}  // namespace daq::modules::universal_renderer
