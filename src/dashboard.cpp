#include "dashboard.h"

#include <coreobjects/callable_info_ptr.h>
#include <coreobjects/eval_value_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <opendaq/component_type_private.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daq::modules::universal_renderer
{

namespace
{

/// The page: a grid of panels, one per signal, filled from /data, which waits
/// for news, so the page shows each change as it comes; and the settings of
/// the device, whose changes need the PIN.
const char* const kPage = R"PAGE(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>TITLE</title>
<style>
body{margin:0;font:14px system-ui,-apple-system,"Segoe UI",sans-serif;background:#eef2f6;color:#222}
header{background:#00335a;color:#fff;padding:10px 18px;display:flex;align-items:center;gap:14px;flex-wrap:wrap}
header h1{font-size:18px;margin:0;font-weight:600}header #s{opacity:.75;font-size:12px}
header nav{display:flex;gap:4px}header button{background:transparent;color:#fff;border:1px solid #ffffff55;border-radius:16px;padding:4px 12px;font:inherit;cursor:pointer}
header button.on{background:#fff;color:#00335a}#lock{margin-left:auto}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:12px;padding:12px}
.card{background:#fff;border-radius:12px;padding:12px 14px;box-shadow:0 1px 2px #0002;position:relative}
.name{font-weight:600;padding-right:22px}.owner{color:#6b6b6b;font-size:12px;min-height:15px}
.x{position:absolute;top:8px;right:8px;border:0;background:none;color:#949494;font-size:18px;cursor:pointer}
.value{font:600 26px ui-monospace,Menlo,Consolas,monospace;margin:6px 0}.unit{font-size:14px;color:#6b6b6b;margin-left:6px}
canvas{width:100%;height:140px;display:block}img{width:100%;border-radius:8px;display:block;background:#111;margin-top:6px}
.range{color:#949494;font-size:11px;display:flex;justify-content:space-between;margin-top:2px}
.empty{grid-column:1/-1;padding:48px;text-align:center;color:#6b6b6b}
.add{display:flex;gap:8px;align-items:center;border:1px dashed #c9d3dd;background:transparent;box-shadow:none}
select,input{font:inherit;padding:5px 8px;border:1px solid #c9d3dd;border-radius:8px;background:#fff;max-width:100%}
.btn{font:inherit;padding:5px 12px;border-radius:16px;border:1px solid #00335a;background:#00335a;color:#fff;cursor:pointer}
#cfg{display:grid;grid-template-columns:minmax(220px,300px) 1fr;gap:12px;padding:12px;align-items:start}
@media(max-width:700px){#cfg{grid-template-columns:1fr}}
#tree,#props{background:#fff;border-radius:12px;box-shadow:0 1px 2px #0002;overflow:hidden}
.node{display:flex;gap:8px;align-items:center;width:100%;border:0;background:none;font:inherit;text-align:left;padding:8px 10px;cursor:pointer;border-top:1px solid #eef2f6}
.node.on{background:#d4f5fa}.node.off span{opacity:.5}.node small{margin-left:auto;color:#949494;font-size:11px}
.dot{width:8px;height:8px;border-radius:50%;background:#1a7f4b;flex:none}.dot.Warning{background:#9a5b00}.dot.Error{background:#b3261e}
#props h2{font-size:16px;margin:0;padding:12px 14px 2px}#props .meta{color:#6b6b6b;font-size:12px;padding:0 14px 10px}
.row{display:flex;align-items:center;gap:12px;padding:8px 14px;border-top:1px solid #eef2f6}
.row label{flex:1;min-width:0}.row label small{display:block;color:#949494;font-size:11px}
.row .ro{font:13px ui-monospace,Menlo,Consolas,monospace;color:#424242;white-space:pre-wrap;text-align:right}
.modes{display:flex;gap:4px}.modes button{font:inherit;padding:4px 10px;border:1px solid #c9d3dd;border-radius:14px;background:#fff;cursor:pointer}.modes button.on{background:#00335a;color:#fff;border-color:#00335a}
[hidden]{display:none!important}
</style></head><body><header><h1>TITLE</h1><nav><button id=tp class=on>Panels</button><button id=ts>Settings</button></nav><span id=s></span><button id=lock>Unlock changes</button></header>
<main id=m></main><section id=cfg hidden><aside id=tree></aside><div id=props></div></section>
<script>
const m=document.getElementById('m'),st=document.getElementById('s'),cfg=document.getElementById('cfg'),tree=document.getElementById('tree'),props=document.getElementById('props'),lockBtn=document.getElementById('lock');
let after=0;const cards=new Map();let pin='';try{pin=sessionStorage.getItem('pin')||''}catch(e){}
function fmt(v){if(v==null||!isFinite(v))return'n/a';const a=Math.abs(v);return a>=10000?String(Math.round(v)):(a<0.01&&a>0)?v.toPrecision(3):v.toFixed(a>=100?1:a>=10?2:3)}
function setPin(p){pin=p;try{p?sessionStorage.setItem('pin',p):sessionStorage.removeItem('pin')}catch(e){}lockBtn.textContent=p?'Changes unlocked':'Unlock changes';lockBtn.className=p?'on':''}
async function unlock(){const p=prompt('The PIN the Device Server app shows:');if(p==null)return false;const r=await fetch('api/unlock',{method:'POST',body:new URLSearchParams({pin:p})});if(r.ok){setPin(p);return true}alert(await r.text());return false}
async function post(path,f){if(!pin&&!await unlock())return false;const r=await fetch(path,{method:'POST',body:new URLSearchParams(Object.assign({},f,{pin:pin}))});
if(r.status===403||r.status===429){setPin('');alert(await r.text());return false}if(!r.ok){alert(await r.text());return false}return true}
lockBtn.onclick=()=>pin?setPin(''):unlock();setPin(pin);
document.getElementById('tp').onclick=()=>view(false);document.getElementById('ts').onclick=()=>view(true);
function view(settings){m.hidden=settings;cfg.hidden=!settings;document.getElementById('tp').className=settings?'':'on';document.getElementById('ts').className=settings?'on':'';if(settings)loadTree()}
function card(i,p){let c=cards.get(i);if(c&&c.kind===p.kind)return c;if(c)c.el.remove();const el=document.createElement('div');el.className='card';
el.innerHTML='<button class=x title="Remove the panel">×</button><div class=name></div><div class=owner></div>'+(p.kind==='picture'?'<img alt="">':'<div class=value></div><canvas></canvas><div class=range><span></span><span></span></div>');
el.querySelector('.x').onclick=()=>post('api/panel/remove',{index:el.dataset.index});
m.insertBefore(el,addCard());c={el:el,kind:p.kind};cards.set(i,c);return c}
let adder=null;function addCard(){if(adder)return adder;adder=document.createElement('div');adder.className='card add';
adder.innerHTML='<select><option value="">Add a panel…</option></select><button class=btn>Add</button>';const sel=adder.querySelector('select');
sel.onfocus=async()=>{const l=await(await fetch('api/signals')).json();sel.length=1;for(const s of l){const o=document.createElement('option');o.value=s.id;o.textContent=(s.owner?s.owner+' · ':'')+s.name;sel.appendChild(o)}};
adder.querySelector('button').onclick=()=>{if(sel.value)post('api/panel',{signal:sel.value})};m.appendChild(adder);return adder}
function line(cv,xs,ys,x0,x1){const r=devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;cv.width=w*r;cv.height=h*r;const g=cv.getContext('2d');g.scale(r,r);
let lo=Infinity,hi=-Infinity;for(const y of ys)if(y!=null&&isFinite(y)){lo=Math.min(lo,y);hi=Math.max(hi,y)}
g.strokeStyle='#e8edf2';g.lineWidth=1;for(let k=1;k<4;k++){const y=Math.round(h*k/4)+.5;g.beginPath();g.moveTo(0,y);g.lineTo(w,y);g.stroke()}
if(!isFinite(lo))return;if(hi===lo){hi+=1;lo-=1}const pad=(hi-lo)*.08;lo-=pad;hi+=pad;
g.strokeStyle='#f15d31';g.lineWidth=1.6;g.lineJoin='round';g.beginPath();let on=false;
for(let k=0;k<xs.length;k++){const y=ys[k];if(y==null||!isFinite(y)){on=false;continue}const px=(xs[k]-x0)/((x1-x0)||1)*w,py=h-(y-lo)/(hi-lo)*h;on?g.lineTo(px,py):g.moveTo(px,py);on=true}g.stroke()}
function show(d){st.textContent=d.panels.length===1?'1 signal':d.panels.length+' signals';addCard();
const e=m.querySelector('.empty');if(!d.panels.length){if(!e){const n=document.createElement('div');n.className='empty';n.textContent='No panel yet. Add a signal below, or connect one to an input port of the Dashboard function block.';m.insertBefore(n,adder)}}else if(e)e.remove();
const seen=new Set();
d.panels.forEach((p,i)=>{seen.add(i);const c=card(i,p);c.el.dataset.index=p.index;c.el.querySelector('.name').textContent=p.name;c.el.querySelector('.owner').textContent=p.owner||'';
if(p.kind==='picture'){const img=c.el.querySelector('img');if(p.picture&&img.dataset.id!=String(p.picture)){img.dataset.id=p.picture;img.src='picture?panel='+i+'&id='+p.picture}return}
c.el.querySelector('.value').innerHTML=fmt(p.last)+'<span class=unit></span>';c.el.querySelector('.unit').textContent=p.unit||'';
const cv=c.el.querySelector('canvas'),rg=c.el.querySelectorAll('.range span');
if(p.kind==='vector'){line(cv,p.x,p.v,p.x[0],p.x[p.x.length-1]);rg[0].textContent=fmt(p.x[0])+' '+(p.xunit||'');rg[1].textContent=fmt(p.x[p.x.length-1])+' '+(p.xunit||'')}
else{line(cv,p.t,p.v,-d.duration,0);rg[0].textContent='−'+d.duration+' s';rg[1].textContent=p.v.length?'min '+fmt(p.min)+' · max '+fmt(p.max):''}});
for(const[i,c]of cards)if(!seen.has(i)){c.el.remove();cards.delete(i)}}
let selected=null;
async function loadTree(){const t=await(await fetch('api/tree',{cache:'no-store'})).json();tree.innerHTML='';
const add=(n,depth)=>{const b=document.createElement('button');b.className='node'+(n.id===selected?' on':'')+(n.active?'':' off');b.style.paddingLeft=(10+depth*16)+'px';
b.innerHTML='<i class=dot></i><span></span><small></small>';b.querySelector('.dot').className='dot '+n.status;b.querySelector('span').textContent=n.name;b.querySelector('small').textContent=n.kind;
b.onclick=()=>{selected=n.id;loadTree();loadProps()};tree.appendChild(b);(n.children||[]).forEach(c=>add(c,depth+1))};add(t,0);if(!selected){selected=t.id;loadProps()}}
async function changed(ok){if(ok){await loadProps();loadTree()}}
async function loadProps(){const d=await(await fetch('api/properties?id='+encodeURIComponent(selected),{cache:'no-store'})).json();props.innerHTML='';
const h=document.createElement('h2');h.textContent=d.name;props.appendChild(h);const meta=document.createElement('div');meta.className='meta';meta.textContent=d.kind+(d.message?' · '+d.message:'');props.appendChild(meta);
const row=(label,hint,control)=>{const r=document.createElement('div');r.className='row';const l=document.createElement('label');l.textContent=label;if(hint){const s=document.createElement('small');s.textContent=hint;l.appendChild(s)}r.appendChild(l);r.appendChild(control);props.appendChild(r)};
if(d.canDeactivate){const c=document.createElement('input');c.type='checkbox';c.checked=d.active;c.onchange=async()=>changed(await post('api/active',{id:selected,active:c.checked}));row('Active','Off stops it',c)}
if(d.modes&&d.modes.length){const g=document.createElement('div');g.className='modes';for(const mode of d.modes){const b=document.createElement('button');b.textContent=mode;b.className=mode===d.mode?'on':'';b.onclick=async()=>changed(await post('api/mode',{id:selected,mode:mode}));g.appendChild(b)}row('Operation mode','',g)}
for(const p of d.properties){let el;const send=async v=>changed(await post('api/property',{id:selected,name:p.name,value:v}));
if(p.readOnly){el=document.createElement('span');el.className='ro';el.textContent=p.type==='selection'?(p.options.find(o=>o[0]==p.value)||[0,p.value])[1]:(p.value==null?'':String(p.value))+(p.unit?' '+p.unit:'')}
else if(p.type==='bool'){el=document.createElement('input');el.type='checkbox';el.checked=!!p.value;el.onchange=()=>send(el.checked)}
else if(p.type==='selection'){el=document.createElement('select');for(const[k,label]of p.options){const o=document.createElement('option');o.value=k;o.textContent=label;el.appendChild(o)}el.value=p.value;el.onchange=()=>send(el.value)}
else if(p.type==='int'||p.type==='float'){el=document.createElement('input');el.type='number';el.step=p.type==='int'?'1':'any';if(p.min!=null)el.min=p.min;if(p.max!=null)el.max=p.max;el.value=p.value;el.style.width='9em';el.onchange=()=>send(el.value)}
else if(p.type==='string'){el=document.createElement('input');el.value=p.value;el.onchange=()=>send(el.value)}
else if(p.type==='procedure'){el=document.createElement('button');el.className='btn';el.textContent='Run';el.onclick=async()=>changed(await post('api/execute',{id:selected,name:p.name}))}
else{el=document.createElement('span');el.className='ro';el.textContent=String(p.value)}
row(p.name+(p.unit&&!p.readOnly?' ['+p.unit+']':''),p.description,el)}}
async function loop(){for(;;){try{const r=await fetch('data?after='+after,{cache:'no-store'});if(r.status===200){const d=await r.json();after=d.sequence;show(d)}}
catch(e){st.textContent='Disconnected; trying again.';await new Promise(f=>setTimeout(f,1000))}}}
loop();
</script></body></html>)PAGE";

/// At most this many points per line: the extremes of as many columns.
constexpr size_t kColumns = 300;

std::string escapeJson(const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += ' ';
                else
                    out += c;
        }
    }
    return out;
}

std::string escapeHtml(const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '&')
            out += "&amp;";
        else
            out += c;
    }
    return out;
}

std::string number(double value)
{
    if (!std::isfinite(value))
        return "null";
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

uint64_t queryNumber(const std::string& query, const char* name)
{
    const std::string key = std::string(name) + "=";
    const auto at = query.find(key);
    if (at == std::string::npos || (at > 0 && query[at - 1] != '&'))
        return 0;
    return std::strtoull(query.c_str() + at + key.size(), nullptr, 10);
}

}  // namespace

FunctionBlockTypePtr DashboardFb::CreateType(const ModuleInfoPtr& moduleInfo)
{
    auto type = FunctionBlockType("UniversalDashboard",
                                  "Dashboard",
                                  "A web page of live panels, one per connected signal, served by the device for any browser on the network");
    if (moduleInfo.assigned())
        checkErrorInfo(type.asPtr<IComponentTypePrivate>(true)->setModuleInfo(moduleInfo));
    return type;
}

DashboardFb::DashboardFb(const ModuleInfoPtr& moduleInfo, const ContextPtr& context, const ComponentPtr& parent, const StringPtr& localId)
    : FunctionBlock(CreateType(moduleInfo), context, parent, localId)
    , frames(std::make_shared<WebFrames>())
{
    initComponentStatus();
    objPtr.addProperty(StringPropertyBuilder("Title", "Dashboard").setDescription("The page's title.").build());
    objPtr.addProperty(FloatPropertyBuilder("Duration", 10.0)
                           .setUnit(Unit("s"))
                           .setMinValue(1.0)
                           .setMaxValue(600.0)
                           .setDescription("The time each panel shows.")
                           .build());
    objPtr.addProperty(BoolPropertyBuilder("WebServer", True).setDescription("Serves the page.").build());
    objPtr.addProperty(IntPropertyBuilder("WebPort", 8090)
                           .setMinValue(1024)
                           .setMaxValue(65535)
                           .setDescription("The port the page is served on.")
                           .build());
    objPtr.addProperty(StringPropertyBuilder("Pin", "")
                           .setDescription("The PIN the page asks for before it changes a setting. Empty: the page only shows.")
                           .build());
    objPtr.addProperty(StringPropertyBuilder("WebAddress", "")
                           .setReadOnly(true)
                           .setDescription("Where to open the page in a browser, or why it is not served.")
                           .build());
    objPtr.getOnPropertyValueWrite("Title") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        std::lock_guard guard(lock);
        title = static_cast<std::string>(args.getValue().asPtr<IString>());
    };
    objPtr.getOnPropertyValueWrite("Pin") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        std::lock_guard guard(lock);
        pin = static_cast<std::string>(args.getValue().asPtr<IString>());
        failures = 0;
    };
    objPtr.getOnPropertyValueWrite("Duration") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        duration = static_cast<double>(args.getValue());
    };
    objPtr.getOnPropertyValueWrite("WebServer") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { applyWeb(); };
    objPtr.getOnPropertyValueWrite("WebPort") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { applyWeb(); };

    updateInputPorts();
    applyWeb();
    worker = std::thread([this] { run(); });
}

DashboardFb::~DashboardFb()
{
    stopping = true;
    if (worker.joinable())
        worker.join();
    if (server)
        server->stop();
}

void DashboardFb::removed()
{
    stopping = true;
    changed.notify_all();
    if (server)
        server->stop();
    FunctionBlock::removed();
}

void DashboardFb::applyWeb()
{
    const bool on = objPtr.getPropertyValue("WebServer");
    const int port = static_cast<int>(static_cast<Int>(objPtr.getPropertyValue("WebPort")));
    std::string address;
    if (server)
        server->stop();
    if (on)
    {
        if (!server)
        {
            server = std::make_unique<WebServer>(frames, "Dashboard");
            server->setRoute([this](const std::string& method,
                                    const std::string& target,
                                    const std::string& query,
                                    const std::string& body,
                                    WebResponse& response) { return answer(method, target, query, body, response); });
        }
        const std::string error = server->start(port);
        if (error.empty())
        {
            for (const auto& url : WebServer::addresses(port))
                address += (address.empty() ? "" : "\n") + url;
            setComponentStatus(ComponentStatus::Ok);
        }
        else
        {
            address = error;
            setComponentStatusWithMessage(ComponentStatus::Warning, error);
        }
    }
    objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue("WebAddress", String(address));
}

void DashboardFb::updateInputPorts()
{
    std::lock_guard guard(lock);
    for (auto it = panels.begin(); it != panels.end();)
    {
        if (!(*it)->port.getSignal().assigned())
        {
            removeInputPort((*it)->port);
            it = panels.erase(it);
        }
        else
        {
            ++it;
        }
    }
    auto panel = std::make_shared<Panel>();
    panel->port = createAndAddInputPort("Input" + std::to_string(portCount++), PacketReadyNotification::None);
    panels.push_back(std::move(panel));
    sequence++;
    changed.notify_all();
}

void DashboardFb::onConnected(const InputPortPtr& /*port*/)
{
    auto configLock = getRecursiveConfigLock();
    updateInputPorts();
}

void DashboardFb::onDisconnected(const InputPortPtr& /*port*/)
{
    auto configLock = getRecursiveConfigLock();
    updateInputPorts();
}

bool DashboardFb::drain(Panel& panel)
{
    const auto connection = panel.port.getConnection();
    if (!connection.assigned())
        return false;
    // The panel is named after the signal and what owns it.
    const auto signal = panel.port.getSignal();
    if (signal.assigned() && signal.getGlobalId().toStdString() != panel.signalId)
    {
        panel.signalId = signal.getGlobalId().toStdString();
        panel.name = signal.getName().toStdString();
        const auto folder = signal.getParent();
        const auto owner = folder.assigned() ? folder.getParent() : nullptr;
        panel.owner = owner.assigned() ? owner.getName().toStdString() : "";
        panel.points.clear();
        panel.hasLast = false;
    }
    bool fresh = false;
    std::vector<double> times;
    std::vector<double> values;
    for (PacketPtr packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (panel.reader.event(packet))
        {
            panel.points.clear();
            fresh = true;
            continue;
        }
        if (packet.getType() != PacketType::Data)
            continue;
        const auto data = packet.asPtr<IDataPacket>(true);
        if (panel.reader.picture)
        {
            const auto* raw = static_cast<const uint8_t*>(data.getRawData());
            if (raw && data.getRawDataSize() > 0)
            {
                panel.picture.assign(raw, raw + data.getRawDataSize());
                panel.pictureId++;
                fresh = true;
            }
            continue;
        }
        panel.reader.read(data, times, values);
        if (times.empty())
            continue;
        fresh = true;
        if (panel.reader.vector)
        {
            const size_t size = panel.reader.vectorSize;
            panel.vectorValues.assign(values.end() - static_cast<std::ptrdiff_t>(size), values.end());
            panel.last = *std::max_element(panel.vectorValues.begin(), panel.vectorValues.end());
            panel.hasLast = true;
            continue;
        }
        for (size_t i = 0; i < times.size(); i++)
            panel.points.emplace_back(times[i], values[i]);
        panel.last = values.back();
        panel.hasLast = true;
        const double keep = panel.points.back().first - duration.load() * 1.02;
        while (!panel.points.empty() && panel.points.front().first < keep)
            panel.points.pop_front();
    }
    return fresh;
}

void DashboardFb::run()
{
    while (!stopping)
    {
        {
            std::lock_guard guard(lock);
            bool fresh = false;
            for (const auto& panel : panels)
                fresh = drain(*panel) || fresh;
            if (fresh)
            {
                sequence++;
                changed.notify_all();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string DashboardFb::json()
{
    // Under the lock.
    const double window = duration;
    std::string out = "{\"sequence\":" + std::to_string(sequence) + ",\"duration\":" + number(window) + ",\"panels\":[";
    bool first = true;
    for (size_t index = 0; index < panels.size(); index++)
    {
        const auto& panel = panels[index];
        if (panel->signalId.empty())
            continue;
        out += first ? "{" : ",{";
        first = false;
        out += "\"index\":" + std::to_string(index) + ",";
        const auto& reader = panel->reader;
        out += "\"name\":\"" + escapeJson(panel->name) + "\",\"owner\":\"" + escapeJson(panel->owner) + "\",\"unit\":\"" + escapeJson(reader.unit) + "\"";
        if (reader.picture)
        {
            out += ",\"kind\":\"picture\",\"picture\":" + std::to_string(panel->pictureId) + "}";
            continue;
        }
        out += ",\"last\":" + (panel->hasLast ? number(panel->last) : std::string("null"));
        if (reader.vector)
        {
            out += ",\"kind\":\"vector\",\"xunit\":\"" + escapeJson(reader.vectorAxisUnit) + "\",\"x\":[";
            const size_t size = panel->vectorValues.size();
            const size_t stride = std::max<size_t>(1, size / 1024);
            std::string xs;
            std::string vs;
            for (size_t i = 0; i < size; i += stride)
            {
                double peak = panel->vectorValues[i];
                for (size_t j = i; j < std::min(size, i + stride); j++)
                    peak = std::max(peak, panel->vectorValues[j]);
                xs += (xs.empty() ? "" : ",") + number(i < reader.vectorAxis.size() ? reader.vectorAxis[i] : static_cast<double>(i));
                vs += (vs.empty() ? "" : ",") + number(peak);
            }
            out += xs + "],\"v\":[" + vs + "]}";
            continue;
        }
        // A line: the last `window` seconds, as times before the newest
        // sample, reduced to the lowest and highest value of each column.
        out += ",\"kind\":\"line\"";
        std::string ts;
        std::string vs;
        double low = INFINITY;
        double high = -INFINITY;
        if (!panel->points.empty())
        {
            const double newest = panel->points.back().first;
            const double start = newest - window;
            const double width = window / kColumns;
            size_t at = 0;
            while (at < panel->points.size() && panel->points[at].first < start)
                at++;
            for (size_t column = 0; column < kColumns && at < panel->points.size(); column++)
            {
                const double end = start + width * static_cast<double>(column + 1);
                double columnLow = INFINITY;
                double columnHigh = -INFINITY;
                double lowTime = 0;
                double highTime = 0;
                for (; at < panel->points.size() && panel->points[at].first <= end; at++)
                {
                    const auto& [t, v] = panel->points[at];
                    if (v < columnLow)
                    {
                        columnLow = v;
                        lowTime = t;
                    }
                    if (v > columnHigh)
                    {
                        columnHigh = v;
                        highTime = t;
                    }
                }
                if (!std::isfinite(columnLow))
                    continue;
                low = std::min(low, columnLow);
                high = std::max(high, columnHigh);
                const bool lowFirst = lowTime <= highTime;
                const double t1 = (lowFirst ? lowTime : highTime) - newest;
                const double t2 = (lowFirst ? highTime : lowTime) - newest;
                const double v1 = lowFirst ? columnLow : columnHigh;
                const double v2 = lowFirst ? columnHigh : columnLow;
                ts += (ts.empty() ? "" : ",") + number(t1) + "," + number(t2);
                vs += (vs.empty() ? "" : ",") + number(v1) + "," + number(v2);
            }
        }
        out += ",\"min\":" + number(low) + ",\"max\":" + number(high) + ",\"t\":[" + ts + "],\"v\":[" + vs + "]}";
    }
    return out + "]}";
}

bool DashboardFb::answer(const std::string& method, const std::string& target, const std::string& query, const std::string& body, WebResponse& response)
{
    if (target.rfind("/api/", 0) == 0)
        return settings(method, target, query, body, response);
    if (target == "/" || target == "/index.html")
    {
        std::string page = kPage;
        std::string name;
        {
            std::lock_guard guard(lock);
            name = escapeHtml(title);
        }
        for (size_t at; (at = page.find("TITLE")) != std::string::npos;)
            page.replace(at, 5, name);
        response.type = "text/html; charset=utf-8";
        response.body = std::move(page);
        return true;
    }
    if (target == "/data")
    {
        // Waits up to a second for news after the sequence the page has.
        const uint64_t after = queryNumber(query, "after");
        std::unique_lock guard(lock);
        changed.wait_for(guard, std::chrono::seconds(1), [&] { return sequence > after || stopping; });
        response.type = "application/json";
        response.body = json();
        return true;
    }
    if (target == "/picture")
    {
        const size_t index = queryNumber(query, "panel");
        std::lock_guard guard(lock);
        size_t shown = 0;
        for (const auto& panel : panels)
        {
            if (panel->signalId.empty())
                continue;
            if (shown++ != index)
                continue;
            if (panel->picture.empty())
                break;
            response.type = panel->reader.pictureFormat == "png" ? "image/png" : "image/jpeg";
            response.body.assign(panel->picture.begin(), panel->picture.end());
            return true;
        }
        response.status = 404;
        response.body = "No picture.\n";
        return true;
    }
    response.status = 404;
    response.body = "Not found. Open / for the dashboard.\n";
    return true;
}

// ---------------------------------------------------------------- settings

namespace
{

/// Wrong PINs before the page is refused for a while, and for how long.
constexpr int kAllowedFailures = 5;
constexpr auto kLockout = std::chrono::seconds(30);

std::string textOf(const BaseObjectPtr& value)
{
    return StringPtr(value.toString()).toStdString();
}

std::string kindOf(const ComponentPtr& component)
{
    if (component.supportsInterface<IDevice>())
        return "device";
    if (component.supportsInterface<IChannel>())
        return "channel";
    if (component.supportsInterface<IFunctionBlock>())
        return "functionBlock";
    if (component.supportsInterface<ISyncComponent>())
        return "synchronization";
    return "component";
}

const char* modeName(OperationModeType mode)
{
    switch (mode)
    {
        case OperationModeType::Idle:
            return "Idle";
        case OperationModeType::Operation:
            return "Operation";
        case OperationModeType::SafeOperation:
            return "SafeOperation";
        default:
            return "Unknown";
    }
}

std::string statusOf(const ComponentPtr& component, std::string* message = nullptr)
{
    try
    {
        const auto container = component.getStatusContainer();
        const auto status = container.getStatus("ComponentStatus");
        if (message)
            *message = container.getStatusMessage("ComponentStatus").toStdString();
        return status.getValue().toStdString();
    }
    catch (...)
    {
        return "Ok";
    }
}

/// The components a settings page lists under one: a device's channels,
/// function blocks, synchronization and devices; a function block's own.
std::vector<ComponentPtr> childrenOf(const ComponentPtr& component)
{
    std::vector<ComponentPtr> out;
    try
    {
        if (const auto device = component.asPtrOrNull<IDevice>(); device.assigned())
        {
            for (const auto& channel : device.getChannels())
                out.push_back(channel);
            for (const auto& block : device.getFunctionBlocks())
                out.push_back(block);
            if (const auto sync = device.getSyncComponent(); sync.assigned())
                out.push_back(sync);
            for (const auto& child : device.getDevices())
                out.push_back(child);
        }
        else if (const auto block = component.asPtrOrNull<IFunctionBlock>(); block.assigned())
        {
            for (const auto& nested : block.getFunctionBlocks())
                out.push_back(nested);
        }
    }
    catch (...)
    {
    }
    return out;
}

std::string treeJson(const ComponentPtr& component, int depth = 0)
{
    std::string out = "{\"id\":\"" + escapeJson(component.getGlobalId().toStdString()) + "\",\"name\":\"" +
                      escapeJson(component.getName().toStdString()) + "\",\"kind\":\"" + kindOf(component) + "\",\"active\":" +
                      (component.getActive() ? "true" : "false") + ",\"status\":\"" + escapeJson(statusOf(component)) + "\",\"children\":[";
    if (depth < 8)
    {
        bool first = true;
        for (const auto& child : childrenOf(component))
        {
            out += (first ? "" : ",") + treeJson(child, depth + 1);
            first = false;
        }
    }
    return out + "]}";
}

ComponentPtr findById(const ComponentPtr& component, const std::string& id, int depth = 0)
{
    if (component.getGlobalId().toStdString() == id)
        return component;
    if (depth >= 8)
        return nullptr;
    for (const auto& child : childrenOf(component))
        if (const auto found = findById(child, id, depth + 1); found.assigned())
            return found;
    return nullptr;
}

std::string valueJson(const BaseObjectPtr& value, CoreType type)
{
    if (!value.assigned())
        return "null";
    try
    {
        switch (type)
        {
            case ctBool:
                return static_cast<bool>(value) ? "true" : "false";
            case ctInt:
                return std::to_string(static_cast<Int>(value));
            case ctFloat:
                return number(static_cast<Float>(value));
            default:
                return "\"" + escapeJson(textOf(value)) + "\"";
        }
    }
    catch (...)
    {
        return "null";
    }
}

std::string failure(WebResponse& response, int status, const std::string& message)
{
    response.status = status;
    response.type = "text/plain; charset=utf-8";
    response.body = message;
    return {};
}

}  // namespace

DevicePtr DashboardFb::owner()
{
    ComponentPtr component = objPtr.asPtr<IComponent>().getParent();
    while (component.assigned() && !component.supportsInterface<IDevice>())
        component = component.getParent();
    return component.assigned() ? component.asPtr<IDevice>() : nullptr;
}

bool DashboardFb::permitted(const std::string& form, WebResponse& response)
{
    std::lock_guard guard(lock);
    const auto now = std::chrono::steady_clock::now();
    if (pin.empty())
    {
        failure(response, 403, "Changes are off: the Dashboard has no PIN.");
        return false;
    }
    if (failures >= kAllowedFailures && now < lockedUntil)
    {
        failure(response, 429, "Too many wrong PINs. Try again in half a minute.");
        return false;
    }
    // Compared in full, so the time taken does not tell how much was right.
    const std::string given = formField(form, "pin");
    unsigned difference = given.size() == pin.size() ? 0 : 1;
    for (size_t i = 0; i < pin.size(); i++)
        difference |= static_cast<unsigned char>(pin[i]) ^ static_cast<unsigned char>(i < given.size() ? given[i] : 0);
    if (difference != 0)
    {
        if (++failures >= kAllowedFailures)
            lockedUntil = now + kLockout;
        failure(response, 403, "The PIN is wrong.");
        return false;
    }
    failures = 0;
    return true;
}

bool DashboardFb::settings(const std::string& method, const std::string& target, const std::string& query, const std::string& body, WebResponse& response)
{
    const auto device = owner();
    if (!device.assigned())
    {
        failure(response, 404, "The Dashboard belongs to no device.");
        return true;
    }
    response.type = "application/json";
    try
    {
        if (method == "GET" && target == "/api/tree")
        {
            response.body = treeJson(device);
            return true;
        }
        if (method == "GET" && target == "/api/signals")
        {
            std::string out = "[";
            for (const auto& signal : device.getSignalsRecursive())
            {
                const auto descriptor = signal.getDescriptor();
                if (!descriptor.assigned() || !signal.getDomainSignal().assigned())
                    continue;
                const auto folder = signal.getParent();
                const auto owner = folder.assigned() ? folder.getParent() : nullptr;
                out += (out.size() > 1 ? ",{" : "{") + std::string("\"id\":\"") + escapeJson(signal.getGlobalId().toStdString()) + "\",\"name\":\"" +
                       escapeJson(signal.getName().toStdString()) + "\",\"owner\":\"" +
                       escapeJson(owner.assigned() ? owner.getName().toStdString() : "") + "\"}";
            }
            response.body = out + "]";
            return true;
        }
        if (method == "GET" && target == "/api/properties")
        {
            const auto component = findById(device, formField(query, "id"));
            if (!component.assigned())
            {
                failure(response, 404, "No such component.");
                return true;
            }
            std::string message;
            const std::string status = statusOf(component, &message);
            const bool self = component.getGlobalId() == objPtr.asPtr<IComponent>().getGlobalId();
            std::string out = "{\"id\":\"" + escapeJson(component.getGlobalId().toStdString()) + "\",\"name\":\"" +
                              escapeJson(component.getName().toStdString()) + "\",\"kind\":\"" + kindOf(component) + "\",\"status\":\"" +
                              escapeJson(status) + "\",\"message\":\"" + escapeJson(message) + "\",\"active\":" +
                              (component.getLocalActive() ? "true" : "false") + ",\"canDeactivate\":" +
                              (component.getGlobalId() == device.getGlobalId() || self ? "false" : "true");
            if (const auto asDevice = component.asPtrOrNull<IDevice>(); asDevice.assigned())
            {
                std::string modes;
                for (const auto& mode : asDevice.getAvailableOperationModes())
                    modes += std::string(modes.empty() ? "" : ",") + "\"" + modeName(static_cast<OperationModeType>(static_cast<Int>(mode))) + "\"";
                out += ",\"mode\":\"" + std::string(modeName(asDevice.getOperationMode())) + "\",\"modes\":[" + modes + "]";
            }
            out += ",\"properties\":[";
            bool first = true;
            for (const auto& property : component.getVisibleProperties())
            {
                const std::string name = property.getName().toStdString();
                const CoreType type = property.getValueType();
                std::string kind;
                std::string extra;
                const auto selection = property.getSelectionValues();
                if (selection.assigned() && type == ctInt)
                {
                    kind = "selection";
                    std::string options;
                    if (const auto values = selection.asPtrOrNull<IList>(); values.assigned())
                    {
                        Int key = 0;
                        for (const auto& label : ListPtr<IBaseObject>(values))
                            options += std::string(options.empty() ? "" : ",") + "[" + std::to_string(key++) + ",\"" + escapeJson(textOf(label)) + "\"]";
                    }
                    else if (const auto dict = selection.asPtrOrNull<IDict>(); dict.assigned())
                    {
                        for (const auto& [key, label] : DictPtr<IBaseObject, IBaseObject>(dict))
                            options += std::string(options.empty() ? "" : ",") + "[" + textOf(key) + ",\"" + escapeJson(textOf(label)) + "\"]";
                    }
                    extra = ",\"options\":[" + options + "]";
                }
                else if (type == ctBool)
                    kind = "bool";
                else if (type == ctInt || type == ctFloat)
                {
                    kind = type == ctInt ? "int" : "float";
                    const auto min = property.getMinValue();
                    const auto max = property.getMaxValue();
                    extra = ",\"min\":" + (min.assigned() ? number(static_cast<Float>(min)) : std::string("null")) +
                            ",\"max\":" + (max.assigned() ? number(static_cast<Float>(max)) : std::string("null"));
                }
                else if (type == ctString)
                    kind = "string";
                else if (type == ctProc)
                {
                    const auto callable = property.getCallableInfo();
                    const auto arguments = callable.assigned() ? callable.getArguments() : nullptr;
                    kind = arguments.assigned() && arguments.getCount() > 0 ? "other" : "procedure";
                }
                else
                    kind = "other";
                // The Dashboard's own server settings stay with openDAQ
                // clients: changing them from the page would cut the page off.
                const bool guarded = self && (name == "WebServer" || name == "WebPort" || name == "Pin" || name == "WebAddress");
                // A procedure is read-only as a value, but can still be run.
                const bool readOnly = (kind != "procedure" && property.getReadOnly()) || guarded || kind == "other";
                std::string value = kind == "procedure" ? "null" : valueJson(component.getPropertyValue(name), type);
                if (self && name == "Pin")
                    value = "\"••••••\"";
                const auto unit = property.getUnit();
                out += std::string(first ? "{" : ",{") + "\"name\":\"" + escapeJson(name) + "\",\"type\":\"" + kind + "\",\"value\":" + value +
                       ",\"readOnly\":" + (readOnly ? "true" : "false") + ",\"unit\":\"" +
                       escapeJson(unit.assigned() ? unit.getSymbol().toStdString() : "") + "\",\"description\":\"" +
                       escapeJson(property.getDescription().assigned() ? property.getDescription().toStdString() : "") + "\"" + extra + "}";
                first = false;
            }
            response.body = out + "]}";
            return true;
        }

        if (method != "POST")
        {
            failure(response, 404, "Not found.");
            return true;
        }
        if (!permitted(body, response))
            return true;
        if (target == "/api/unlock")
        {
            response.body = "{\"ok\":true}";
            return true;
        }
        if (target == "/api/panel" || target == "/api/panel/remove")
        {
            if (target == "/api/panel")
            {
                const std::string id = formField(body, "signal");
                for (const auto& signal : device.getSignalsRecursive())
                {
                    if (signal.getGlobalId().toStdString() != id)
                        continue;
                    InputPortConfigPtr free;
                    {
                        std::lock_guard guard(lock);
                        for (const auto& panel : panels)
                            if (!panel->port.getSignal().assigned())
                                free = panel->port;
                    }
                    if (free.assigned())
                        free.connect(signal);
                    break;
                }
            }
            else
            {
                const size_t index = std::strtoul(formField(body, "index").c_str(), nullptr, 10);
                InputPortConfigPtr port;
                {
                    std::lock_guard guard(lock);
                    if (index < panels.size())
                        port = panels[index]->port;
                }
                if (port.assigned())
                    port.disconnect();
            }
            response.body = "{\"ok\":true}";
            return true;
        }

        const auto component = findById(device, formField(body, "id"));
        if (!component.assigned())
        {
            failure(response, 404, "No such component.");
            return true;
        }
        if (target == "/api/active")
        {
            component.setActive(formField(body, "active") == "true");
        }
        else if (target == "/api/mode")
        {
            const std::string mode = formField(body, "mode");
            const auto asDevice = component.asPtr<IDevice>();
            asDevice.setOperationMode(mode == "Idle"        ? OperationModeType::Idle
                                      : mode == "Operation" ? OperationModeType::Operation
                                                            : OperationModeType::SafeOperation);
        }
        else if (target == "/api/property" || target == "/api/execute")
        {
            const std::string name = formField(body, "name");
            const bool self = component.getGlobalId() == objPtr.asPtr<IComponent>().getGlobalId();
            if (!component.hasProperty(name) || (self && (name == "WebServer" || name == "WebPort" || name == "Pin")))
            {
                failure(response, 400, "This setting cannot be changed from the page.");
                return true;
            }
            const auto property = component.getProperty(name);
            const CoreType type = property.getValueType();
            if (target == "/api/execute")
            {
                if (type != ctProc)
                {
                    failure(response, 400, name + " is not a procedure.");
                    return true;
                }
                const ProcedurePtr procedure = component.getPropertyValue(name);
                procedure();
            }
            else
            {
                const std::string text = formField(body, "value");
                if (type == ctBool)
                    component.setPropertyValue(name, Boolean(text == "true"));
                else if (type == ctInt)
                    component.setPropertyValue(name, Integer(std::stoll(text)));
                else if (type == ctFloat)
                    component.setPropertyValue(name, Floating(std::stod(text)));
                else if (type == ctString)
                    component.setPropertyValue(name, String(text));
                else
                {
                    failure(response, 400, name + " cannot be changed from the page.");
                    return true;
                }
            }
        }
        else
        {
            failure(response, 404, "Not found.");
            return true;
        }
        response.body = "{\"ok\":true}";
    }
    catch (const std::exception& error)
    {
        failure(response, 400, error.what());
    }
    return true;
}

}  // namespace daq::modules::universal_renderer
