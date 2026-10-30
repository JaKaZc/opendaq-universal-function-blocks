#include "web.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using socklen_t = int;
#define closeSocket closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define closeSocket close
#endif

namespace daq::modules::universal_renderer
{

namespace
{

// ---------------------------------------------------------------- the page

// The page draws the plot data itself, the same way openDAQ Studio does:
// lines, filled areas, texts and pictures, scaled to the window.
const char* const kPage = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TITLE</title>
<style>html,body{margin:0;height:100%;background:#101418;color:#ccd;font:13px system-ui,sans-serif}
canvas{position:fixed;inset:0;width:100%;height:100%}#i{position:fixed;right:8px;bottom:6px;opacity:.7}
#i a{color:#9bd}</style></head><body><canvas id="c"></canvas>
<div id="i"><span id="r"></span> · <a href="stream">pictures</a></div>
<script>
const c=document.getElementById('c'),g=c.getContext('2d'),r=document.getElementById('r');
let data=null,after=0,times=[];const slots=new Map(),pending=new Set();
function picture(slot,id,bytes,type){const s=slots.get(slot);if(s&&s.id===id)return s.image;
 if(!pending.has(id)){pending.add(id);createImageBitmap(new Blob([bytes.slice()],{type})).then(im=>{pending.delete(id);
 const old=slots.get(slot);if(old&&old.id>id){im.close();return}if(old)old.image.close();slots.set(slot,{id,image:im});draw()}).catch(()=>pending.delete(id))}
 return s?s.image:null}
const td=new TextDecoder();
function draw(){if(!data)return;const v=new DataView(data),W=innerWidth,H=innerHeight,dpr=devicePixelRatio||1;
 if(c.width!==Math.round(W*dpr)||c.height!==Math.round(H*dpr)){c.width=Math.round(W*dpr);c.height=Math.round(H*dpr)}
 if(v.getUint32(0,true)!==0x4c50444f)return;const pw=v.getUint16(8,true),ph=v.getUint16(10,true),n=v.getUint32(16,true);
 const k=Math.min(W/pw,H/ph),ox=(W-pw*k)/2,oy=(H-ph*k)/2;g.setTransform(dpr,0,0,dpr,0,0);g.clearRect(0,0,W,H);
 g.setTransform(dpr*k,0,0,dpr*k,dpr*ox,dpr*oy);g.fillStyle=`rgb(${v.getUint8(12)},${v.getUint8(13)},${v.getUint8(14)})`;g.fillRect(0,0,pw,ph);
 g.lineJoin='round';g.lineCap='round';let a=20,slot=0;const xy=o=>v.getInt16(o,true)/8;
 const path=move=>{const m=v.getUint32(a,true);a+=4;for(let i=0;i<m;i++){const x=xy(a),y=xy(a+2);i===0&&move?g.moveTo(x,y):g.lineTo(x,y);a+=4}return m};
 for(let op=0;op<n&&a<data.byteLength;op++){const t=v.getUint8(a),col=`rgb(${v.getUint8(a+1)},${v.getUint8(a+2)},${v.getUint8(a+3)})`;a+=4;
  if(t===1){const dash=v.getUint8(a)===1,th=v.getUint16(a+1,true)/16;a+=3;g.beginPath();path(true);g.strokeStyle=col;g.lineWidth=Math.max(th,.5);g.setLineDash(dash?[6,4]:[]);g.stroke()}
  else if(t===2){const al=v.getUint8(a)/255,base=xy(a+1);a+=3;const s=a;g.beginPath();g.moveTo(xy(s+4),base);const m=path(false);g.lineTo(xy(s+4+(m-1)*4),base);g.closePath();g.globalAlpha=al;g.fillStyle=col;g.fill();g.globalAlpha=1}
  else if(t===3){const f=v.getUint8(a),x=xy(a+1),y=xy(a+3),len=v.getUint16(a+5,true),s=td.decode(new Uint8Array(data,a+7,len));a+=7+len;
   g.font=(f&1?'bold ':'')+'16px "DejaVu Sans",Verdana,sans-serif';g.textAlign=['left','center','right'][(f>>1)&3];g.textBaseline='alphabetic';g.fillStyle=col;g.fillText(s,x,y+15)}
  else if(t===4){const fmt=v.getUint8(a),id=v.getUint32(a+1,true),x=xy(a+5),y=xy(a+7),w=xy(a+9),h=xy(a+11),len=v.getUint32(a+13,true);
   const im=picture(slot++,id,new Uint8Array(data,a+17,len),fmt===2?'image/png':'image/jpeg');a+=17+len;if(im)g.drawImage(im,x,y,w,h)}
  else break}}
addEventListener('resize',draw);
async function loop(){for(;;){try{const res=await fetch('plot?after='+after,{cache:'no-store'});after=+(res.headers.get('X-Sequence')||after);
 if(res.status===200){data=await res.arrayBuffer();const now=performance.now();times.push(now);while(times.length&&now-times[0]>2000)times.shift();
 r.textContent=(times.length>1?((times.length-1)*1000/(now-times[0])).toFixed(1):'0')+' frames/s';requestAnimationFrame(draw)}}
 catch(e){r.textContent='reconnecting';await new Promise(f=>setTimeout(f,1000))}}}
loop();
</script></body></html>
)PAGE";

bool sendAll(int socket, const void* data, size_t length)
{
    const char* at = static_cast<const char*>(data);
    while (length > 0)
    {
#if defined(MSG_NOSIGNAL)
        const auto sent = ::send(socket, at, length, MSG_NOSIGNAL);
#else
        const auto sent = ::send(socket, at, static_cast<int>(length), 0);
#endif
        if (sent <= 0)
            return false;
        at += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

bool sendText(int socket, const std::string& text)
{
    return sendAll(socket, text.data(), text.size());
}

/// Waits up to `milliseconds` for the socket to have something to read.
bool readable(int socket, int milliseconds)
{
#if defined(_WIN32)
    WSAPOLLFD entry{static_cast<SOCKET>(socket), POLLIN, 0};
    return WSAPoll(&entry, 1, milliseconds) > 0;
#else
    pollfd entry{socket, POLLIN, 0};
    return ::poll(&entry, 1, milliseconds) > 0;
#endif
}

std::string escapeHtml(const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

uint64_t queryNumber(const std::string& query, const char* name)
{
    const std::string key = std::string(name) + "=";
    const auto at = query.find(key);
    if (at == std::string::npos)
        return 0;
    return std::strtoull(query.c_str() + at + key.size(), nullptr, 10);
}

std::string header(int status, const char* reason, const char* type, size_t length, uint64_t sequence, bool keepAlive)
{
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    if (type)
        out += std::string("Content-Type: ") + type + "\r\n";
    out += "Content-Length: " + std::to_string(length) + "\r\n";
    out += "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Sequence\r\n";
    out += "X-Sequence: " + std::to_string(sequence) + "\r\n";
    out += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    return out;
}

}  // namespace

std::string formField(const std::string& form, const std::string& name)
{
    size_t at = 0;
    while (at <= form.size())
    {
        const size_t end = std::min(form.find('&', at), form.size());
        const size_t equals = form.find('=', at);
        if (equals != std::string::npos && equals < end && form.compare(at, equals - at, name) == 0 && equals - at == name.size())
        {
            std::string out;
            for (size_t i = equals + 1; i < end; i++)
            {
                if (form[i] == '+')
                    out += ' ';
                else if (form[i] == '%' && i + 2 < end + 1 && std::isxdigit(static_cast<unsigned char>(form[i + 1])) &&
                         std::isxdigit(static_cast<unsigned char>(form[i + 2])))
                {
                    out += static_cast<char>(std::stoi(form.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else
                    out += form[i];
            }
            return out;
        }
        at = end + 1;
    }
    return {};
}

// ---------------------------------------------------------------- frames

void WebFrames::publishPlot(std::vector<uint8_t> bytes)
{
    {
        std::lock_guard guard(lock);
        plot = std::move(bytes);
        plotSequence++;
    }
    changed.notify_all();
}

void WebFrames::publishPicture(std::vector<uint8_t> bytes)
{
    {
        std::lock_guard guard(lock);
        picture = std::move(bytes);
        pictureSequence++;
    }
    changed.notify_all();
}

bool WebFrames::waitPlot(uint64_t after, std::chrono::milliseconds timeout, std::vector<uint8_t>& bytes, uint64_t& sequence)
{
    std::unique_lock guard(lock);
    changed.wait_for(guard, timeout, [&] { return released || plotSequence > after; });
    sequence = plotSequence;
    if (released || plotSequence <= after || plot.empty())
        return false;
    bytes = plot;
    return true;
}

bool WebFrames::waitPicture(uint64_t after, std::chrono::milliseconds timeout, std::vector<uint8_t>& bytes, uint64_t& sequence)
{
    std::unique_lock guard(lock);
    pictureAsked = std::chrono::steady_clock::now();
    changed.wait_for(guard, timeout, [&] { return released || pictureSequence > after; });
    sequence = pictureSequence;
    if (released || pictureSequence <= after || picture.empty())
        return false;
    bytes = picture;
    return true;
}

bool WebFrames::picturesWanted()
{
    std::lock_guard guard(lock);
    return streams > 0 || std::chrono::steady_clock::now() - pictureAsked < std::chrono::seconds(2);
}

void WebFrames::wantPictures()
{
    std::lock_guard guard(lock);
    pictureAsked = std::chrono::steady_clock::now();
}

void WebFrames::streamOpened()
{
    std::lock_guard guard(lock);
    streams++;
}

void WebFrames::streamClosed()
{
    std::lock_guard guard(lock);
    streams--;
}

void WebFrames::release()
{
    {
        std::lock_guard guard(lock);
        released = true;
    }
    changed.notify_all();
}

void WebFrames::renew()
{
    std::lock_guard guard(lock);
    released = false;
}

// ---------------------------------------------------------------- server

WebServer::WebServer(std::shared_ptr<WebFrames> frames, std::string title)
    : frames(std::move(frames))
    , title(std::move(title))
{
#if defined(_WIN32)
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

WebServer::~WebServer()
{
    stop();
}

std::string WebServer::start(int port)
{
    stop();
    const int socket = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (socket < 0)
        return "The web server could not open a socket.";
    int yes = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(socket, 16) != 0)
    {
        closeSocket(socket);
        return "The web server could not listen on port " + std::to_string(port) + "; another program may use it.";
    }
    listener = socket;
    stopping = false;
    frames->renew();
    acceptor = std::thread([this] { acceptLoop(); });
    return {};
}

void WebServer::stop()
{
    if (listener < 0)
        return;
    stopping = true;
    if (acceptor.joinable())
        acceptor.join();
    closeSocket(listener);
    listener = -1;
    // Clients waiting for a frame are woken; open connections are cut.
    frames->release();
    std::unique_lock guard(clientsLock);
    for (const int client : clients)
        ::shutdown(client, 2);
    clientsDone.wait_for(guard, std::chrono::seconds(10), [this] { return clients.empty(); });
}

void WebServer::acceptLoop()
{
    while (!stopping)
    {
        if (!readable(listener, 200))
            continue;
        const int client = static_cast<int>(::accept(listener, nullptr, nullptr));
        if (client < 0)
            continue;
        int yes = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#if defined(SO_NOSIGPIPE)
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
        {
            std::lock_guard guard(clientsLock);
            clients.insert(client);
        }
        std::thread([this, client] { serve(client); }).detach();
    }
}

void WebServer::serve(int socket)
{
    std::string buffer;
    char chunk[2048];
    bool keepAlive = true;
    while (keepAlive && !stopping)
    {
        // One request: its header, up to the empty line.
        size_t end;
        while ((end = buffer.find("\r\n\r\n")) == std::string::npos)
        {
            if (buffer.size() > 8192 || stopping || !readable(socket, 15000))
                goto done;
            const auto got = ::recv(socket, chunk, sizeof(chunk), 0);
            if (got <= 0)
                goto done;
            buffer.append(chunk, static_cast<size_t>(got));
        }
        const std::string request = buffer.substr(0, end);
        buffer.erase(0, end + 4);
        // A body, for a POST: as long as Content-Length says, at most 64 kB.
        std::string body;
        {
            const std::string lowered = lower(request);
            const auto at = lowered.find("content-length:");
            const size_t length = at == std::string::npos ? 0 : std::strtoul(request.c_str() + at + 15, nullptr, 10);
            if (length > 65536)
                break;
            while (buffer.size() < length)
            {
                if (stopping || !readable(socket, 15000))
                    goto done;
                const auto got = ::recv(socket, chunk, sizeof(chunk), 0);
                if (got <= 0)
                    goto done;
                buffer.append(chunk, static_cast<size_t>(got));
            }
            body = buffer.substr(0, length);
            buffer.erase(0, length);
        }
        keepAlive = lower(request).find("connection: close") == std::string::npos;

        const auto firstSpace = request.find(' ');
        const auto secondSpace = request.find(' ', firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos)
            break;
        const std::string method = request.substr(0, firstSpace);
        std::string target = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        std::string query;
        if (const auto mark = target.find('?'); mark != std::string::npos)
        {
            query = target.substr(mark + 1);
            target.resize(mark);
        }

        WebResponse answer;
        if (route && (method == "GET" || method == "POST") && route(method, target, query, body, answer))
        {
            const char* reason = answer.status == 200   ? "OK"
                                 : answer.status == 204 ? "No Content"
                                 : answer.status == 403 ? "Forbidden"
                                 : answer.status == 404 ? "Not Found"
                                 : answer.status == 429 ? "Too Many Requests"
                                                        : "Bad Request";
            if (!sendText(socket, header(answer.status, reason, answer.status == 204 ? nullptr : answer.type.c_str(), answer.body.size(), 0, keepAlive)) ||
                !sendAll(socket, answer.body.data(), answer.body.size()))
                break;
        }
        else if (method != "GET")
        {
            const std::string text = "Only GET is served.\n";
            sendText(socket, header(405, "Method Not Allowed", "text/plain", text.size(), 0, keepAlive) + text);
        }
        else if (target == "/" || target == "/index.html")
        {
            std::string page = kPage;
            page.replace(page.find("TITLE"), 5, escapeHtml(title));
            if (!sendText(socket, header(200, "OK", "text/html; charset=utf-8", page.size(), 0, keepAlive) + page))
                break;
        }
        else if (target == "/plot")
        {
            std::vector<uint8_t> bytes;
            uint64_t sequence = 0;
            if (frames->waitPlot(queryNumber(query, "after"), std::chrono::milliseconds(1000), bytes, sequence))
            {
                if (!sendText(socket, header(200, "OK", "application/x-opendaq-plot", bytes.size(), sequence, keepAlive)) ||
                    !sendAll(socket, bytes.data(), bytes.size()))
                    break;
            }
            else if (!sendText(socket, header(204, "No Content", nullptr, 0, sequence, keepAlive)))
            {
                break;
            }
        }
        else if (target == "/picture")
        {
            // The newest picture; pictures are made while someone asks.
            frames->wantPictures();
            std::vector<uint8_t> bytes;
            uint64_t sequence = 0;
            const uint64_t after = queryNumber(query, "after");
            if (frames->waitPicture(after, std::chrono::milliseconds(after ? 1000 : 2000), bytes, sequence))
            {
                if (!sendText(socket, header(200, "OK", "image/png", bytes.size(), sequence, keepAlive)) ||
                    !sendAll(socket, bytes.data(), bytes.size()))
                    break;
            }
            else if (!sendText(socket, header(204, "No Content", nullptr, 0, sequence, keepAlive)))
            {
                break;
            }
        }
        else if (target == "/stream")
        {
            // Each new picture replaces the last, for as long as the client
            // keeps the connection.
            frames->streamOpened();
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                     "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
            uint64_t sequence = 0;
            if (sendText(socket, head))
            {
                std::vector<uint8_t> bytes;
                while (!stopping)
                {
                    if (!frames->waitPicture(sequence, std::chrono::milliseconds(1000), bytes, sequence))
                        continue;
                    const std::string part =
                        "--frame\r\nContent-Type: image/png\r\nContent-Length: " + std::to_string(bytes.size()) + "\r\n\r\n";
                    if (!sendText(socket, part) || !sendAll(socket, bytes.data(), bytes.size()) || !sendText(socket, "\r\n"))
                        break;
                }
            }
            frames->streamClosed();
            break;
        }
        else
        {
            const std::string text = "Not found. Open / for the plot, /stream for pictures.\n";
            if (!sendText(socket, header(404, "Not Found", "text/plain", text.size(), 0, keepAlive) + text))
                break;
        }
    }
done:
    closeSocket(socket);
    {
        std::lock_guard guard(clientsLock);
        clients.erase(socket);
    }
    clientsDone.notify_all();
}

std::vector<std::string> WebServer::addresses(int port)
{
    std::vector<std::string> out;
#if !defined(_WIN32)
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0)
    {
        for (ifaddrs* entry = list; entry; entry = entry->ifa_next)
        {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_LOOPBACK) || !(entry->ifa_flags & IFF_UP))
                continue;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr, text, sizeof(text));
            out.push_back("http://" + std::string(text) + ":" + std::to_string(port) + "/");
        }
        freeifaddrs(list);
    }
#endif
    if (out.empty())
        out.push_back("http://localhost:" + std::to_string(port) + "/");
    return out;
}

}  // namespace daq::modules::universal_renderer
