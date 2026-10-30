# openDAQ Universal Function Blocks

An [openDAQ](https://opendaq.com) module, **UniversalFunctionBlocksModule**, with six
function blocks for presenting and evaluating data. They run on any openDAQ
device, such as a phone, so every client of the device gets their results:

| Function block | Type ID | What it does |
|---|---|---|
| **Universal renderer** | `UniversalRenderer` | Plots its input signals and pictures, as plot data for openDAQ Studio, as pictures for any client, and on a web page |
| **Level** | `UniversalLevel` | RMS, peak and level in dB of a signal over a window |
| **Spectrum** | `UniversalSpectrum` | The amplitude spectrum of a signal, by FFT |
| **Vibration** | `UniversalVibration` | Vibration severity as ISO 10816 measures it, from up to three acceleration axes |
| **Sound level meter** | `UniversalSoundLevelMeter` | A sound level meter after IEC 61672, with octave and third-octave bands |
| **Dashboard** | `UniversalDashboard` | A web page of live panels for any browser on the network, with a Settings view protected by a PIN |

The module is C++17 and depends only on the openDAQ SDK. Its plot data,
drawing, font, PNG encoder, JPEG decoder, FFT and web servers are its own, so
it builds the same for Android, macOS, Linux and Windows. The library is
`libuniversal_function_blocks_module.module.so` (`.dylib` on macOS,
`universal_function_blocks_module.module.dll` on Windows).

## The openDAQ phone projects

This repository is one of four that work together, but each one stands on
its own: take only the parts you need, and build your own around them.
"Needs" says what a repository cannot do without.

| Repository | What it is | Needs |
|---|---|---|
| [opendaq-android-device-module](https://github.com/JaKaZc/opendaq-android-device-module) | openDAQ module, **Android only**: the phone's sensors and state as one device, `android://` | The openDAQ SDK and the Android NDK |
| **opendaq-universal-function-blocks** (this one) | openDAQ module, UniversalFunctionBlocksModule: Universal renderer, Level, Spectrum, Vibration, Sound level meter and Dashboard, for any platform | The openDAQ SDK |
| [opendaq-device-server](https://github.com/JaKaZc/opendaq-device-server) | Android app that serves the phone as an openDAQ device on the network | The Android device module. The function blocks module is optional |
| [opendaq-studio-desktop](https://github.com/JaKaZc/opendaq-studio-desktop) | openDAQ Studio, the desktop client for macOS and Windows, for any openDAQ device | Nothing else. The function blocks module is optional |

```
 Android phone: opendaq-device-server                 Computer: opendaq-studio-desktop
 ┌──────────────────────────────────────────┐         ┌──────────────────────────────────────────┐
 │ openDAQ instance                         │         │ openDAQ Studio                           │
 │  android:// device  <- Android device    │   TCP   │  openDAQ instance, native client         │
 │                        module            │   7420  │  this module, optional, so the blocks    │
 │  function blocks    <- this module       │ ──────▶ │  can be added on the computer too        │
 │  native streaming server, mDNS           │         │                                          │
 └──────────────────────────────────────────┘         └──────────────────────────────────────────┘
```

The module needs none of the other repositories, and it does not need a
phone: it works in any openDAQ application, on any platform the SDK runs on.
In these projects it is used in two places:

- **On the phone**, the Device Server loads it. The phone's device adds Sound
  level, Sound spectrum and Vibration blocks to its own signals, a Dashboard,
  and one Universal renderer that a client can connect to any signal.
- **On the computer**, openDAQ Studio can ship it, so it can add the blocks
  to the devices it runs itself, such as the computer's sound card. Studio
  configures the phone's blocks with or without it, because the phone serves
  their signals, ports and properties, and it draws the renderer's plot data
  itself.

All four repositories are built against the same openDAQ SDK release,
**v3.40.3**. A module must be built against the release that loads it.

## Use only what you need

- **In any openDAQ application.** Build the module against the SDK release
  of the application and load it from its folder (see Building). Its blocks
  then work on any device of that instance, and every client of the instance
  reads their outputs.
- **Without openDAQ Studio.** The renderer's `Picture` output is plain PNG,
  which any openDAQ client can show, and its web server serves any browser.
  The `Plot` output is plot data, a small binary format documented under
  [Plot data](#plot-data), so another viewer can draw it too.
- **Only some of the blocks.** Each block is its own class, registered in
  `src/module.cpp`. Remove the ones you do not want there, or add blocks of
  your own the same way.
- **On a phone without the Device Server.** The
  [Android device module](https://github.com/JaKaZc/opendaq-android-device-module) uses the module's
  blocks when they are loaded and works without them.

## Universal renderer

An openDAQ module with one function block, **Universal renderer**
(`UniversalRenderer`). It lays out the same plot as the SDK's
reference renderer (`RefFBModuleRenderer`), with the same properties, and
shows pictures such as a camera's in it. Instead of drawing in a window, it
offers the plot in three ways:

| Output | For | Cost |
|---|---|---|
| `Plot` signal: plot data | openDAQ Studio, desktop and phone, which draws it sharp at any size | About 2 to 25 kB a plot; 240 a second take 5% of a core |
| `Picture` signal: PNG pictures | Any openDAQ client that shows pictures | Made only while a client reads them, at `PictureFrameRate` |
| A small web server | Any web browser on the network, and tools that read HTTP | Run with `WebServer`; pictures only while someone watches them |

The reference renderer opens an SFML window on the machine that runs openDAQ.
Android cannot open one, and macOS opens one only from the main thread, so
openDAQ Studio cannot use it. The Universal renderer needs no window and no
graphics driver, and its outputs travel over the network like any openDAQ
data.

It is C++17 and depends only on the openDAQ SDK: its plot data, drawing, font,
PNG encoder, JPEG decoder and web server are its own, so it builds the same for
Android, macOS, Linux and Windows.

### Using it

1. Add the function block **Universal renderer**.
2. Connect signals to its input ports: numbers, vectors such as a spectrum, or
   pictures such as a camera's `Image`. As with the reference renderer, a new
   free input port appears whenever one is connected, and a disconnected port
   is removed.
3. In openDAQ Studio, show the output signal `Plot`, or press **OpenWindow**.
   Any other client shows `Picture`. A browser opens the address in
   `WebAddress` once `WebServer` is on.

### Properties

| Property | Default | Meaning |
|---|---|---|
| Duration | 1 s | The time the plot shows |
| SingleXAxis | off | All signals on one time axis |
| SingleYAxis | off | With SingleXAxis, all signals in one chart on one value axis |
| LineThickness | 1 | Line width in pixels |
| Freeze | off | Holds the plot while data keeps arriving |
| Resolution | 800x600 | The plot's size, which sets its shape; viewers scale it |
| ShowLastValue | off | Shows each signal's newest value |
| UseCustomMinMaxValue | off | Uses CustomMinValue and CustomMaxValue as the value axis |
| CustomMaxValue, CustomMinValue | 10, -10 | The custom value axis |
| UseCustom2dRangeValue | off | Draws only part of a vector signal, such as a spectrum |
| Custom2dMinRange, Custom2dMaxRange | 0, 100 | The elements drawn |
| OpenWindow | | Opens the plot in a window of its own, in the openDAQ Studio that runs it |
| View | 2D | 2D, as the reference renderer, or 3D |
| FrameRate | 25 fps | The most plots a second, 1 to 240 |
| PictureFrameRate | 10 fps | The most pictures a second, 1 to 240, at most one per plot |
| WebServer | off | Runs the web server |
| WebPort | 8080 | The web server's port |
| WebAddress | | Read only: where to open the plot in a browser, or why the server did not start |

The properties from Duration to Custom2dMaxRange, and OpenWindow, are the
reference renderer's; its `UseMainLoopForRenderer` configuration is left out,
because there is no window. The block has no screen, so OpenWindow does
nothing on the device: openDAQ Studio opens the `Plot` signal in a window when
it is run from it, a window on a computer and a full-screen page on a phone.

In **3D**, the scalar signals share one time axis, and each is a filled
surface on a plane of its own, one behind the other, the first connected at
the back; the surfaces in front hide those behind. A vector signal such as a
spectrum becomes a waterfall: its values over the duration, the newest at the
front. All other properties apply in both views.

### Pictures as inputs

A signal of sample type `Binary` whose unit is `jpeg` or `png`, openDAQ's video
convention, as the Android device module's camera sends it, is shown as a
picture: in the lower half of the plot, or the whole plot when nothing else is
connected, with its title, keeping its proportions; several sit side by side.
The newest picture is shown.

- In the plot data, the picture travels as it came, and the viewer decodes it.
- In the Picture output and the web server's pictures, the renderer decodes
  it itself. It reads baseline JPEG, which cameras write, and turns the picture
  as its EXIF orientation says, as phone cameras need. Progressive JPEG and PNG
  inputs are drawn only in the plot data; the pictures show a note instead.

### Web server

With `WebServer` on, the block listens on `WebPort` on every network
interface, and `WebAddress` lists the addresses to open, such as
`http://192.168.1.39:8080/`. It serves:

| Path | What |
|---|---|
| `/` | A page that draws the plot data in the browser, at the frame rate, sharp at any window size |
| `/stream` | The pictures as a live stream (`multipart/x-mixed-replace`), which browsers show without JavaScript and video tools can open |
| `/picture` | The newest picture, PNG |
| `/plot` | The newest plot data; `?after=N` waits up to a second for one newer than number N, given in the `X-Sequence` header |

The server is plain sockets and HTTP/1.1 with keep-alive, about 550 lines of
C-style code, with no library. It answers any origin, so other web pages can
read `/plot`. It only shows: nothing can be changed through it, but anyone on
the network can watch while it runs, so run it only on a network you trust.
Pictures are made only while a client of `/stream` or `/picture` watches.

### How it works

1. **Reading.** A render thread wakes `FrameRate` times a second and takes
   what queued on each input port: descriptor changes, which say the sample
   type, unit, value range and time domain, and data packets. Each sample's
   time is computed from its domain (start + delta × i, or read), in seconds
   since 1970 when the domain has a time origin. The block keeps the last
   `Duration` of every signal, of a vector signal its last vectors, and of a
   picture signal its newest picture.
2. **Layout.** It places lanes or a 3D box, axes, grid, labels, titles and
   pictures, with the reference renderer's rules, and turns the samples into
   lines. Many samples in one pixel column are reduced to the column's first,
   lowest, highest and last value, which draws the same line.
3. **Plot data.** The drawing is recorded as plot data, and sent as one
   sample on `Plot`, with its time on `PlotTime`, and to the web server.
4. **Pictures.** While a client reads `Picture`, or a web client watches
   pictures, the layout is recorded once and handed to a picture thread of its
   own. That thread draws it into pixels, encodes them as PNG on up to four
   cores, and sends the picture on `Picture`, with its time on `PictureTime`.
   The plot never waits for a picture: a layout handed over while the thread
   is busy replaces the one waiting. A client reading the signal connects to
   it, also through openDAQ's streaming server for a remote client, so the
   block knows.

While data flows on a time axis with an origin, the axis follows the
computer's clock between the device's packets, so the plot scrolls at the
frame rate instead of jumping once per packet. A plot is sent only when
something changed, and once a second while nothing does, so a viewer that
starts watching gets one.

### Plot data

The output signal has sample type `Binary`, unit `plot`, and the plot's
`width` and `height` in its descriptor's metadata. Each sample is one plot,
little-endian:

| Bytes | Field |
|---|---|
| 4 | `ODPL` |
| 1 | Version, 1 |
| 3 | Reserved |
| 2, 2 | Width and height of the plot's coordinate space |
| 3, 1 | Background color, RGB, and a reserved byte |
| 4 | Number of operations |

Then the operations, each starting with its type and an RGB color. A
coordinate is a signed 16-bit number in eighths of a pixel; points are a
32-bit count followed by that many x, y pairs.

| Type | Fields after the color |
|---|---|
| 1, line | style (0 solid, 1 dashed 6 on, 4 off), thickness (16-bit, sixteenths of a pixel), points |
| 2, area | alpha (0 to 255), baseline y, points: the area between the line and the level baseline |
| 3, text | flags (bit 0 bold; bits 1 and 2 alignment: 0 left, 1 centre, 2 right), x, top of the line, 16-bit length, UTF-8 |
| 4, picture | format (1 JPEG, 2 PNG), 32-bit id, x, y, width, height, 32-bit length, the picture's bytes |

Texts are laid out for DejaVu Sans at 16 pixels, whose baseline is 15 pixels
below the top of its line; a viewer draws them in a font of that size. A
picture's id changes with each new picture, so a viewer decodes each once;
the color of a picture operation is unused. The operations are drawn in
order, so later ones cover earlier ones.

## The other function blocks

The module has three more function blocks, which run on any device, such as
a phone, so every client gets their results:

| Function block | What it does | Properties |
|---|---|---|
| **Level** (`UniversalLevel`) | RMS, peak and level in dB of one input signal over a window: outputs `Rms`, `Peak` and `Level`, one value per window | Window (0.1 s), Reference (the RMS that is 0 dB; 1 gives dB of full scale for a microphone) |
| **Spectrum** (`UniversalSpectrum`) | The amplitude spectrum of an input with a fixed sample rate, by FFT with a Hann window over blocks that overlap by half: output `Spectrum`, an array per block whose elements are the frequencies from 0 to half the sample rate | BlockSize (2048), Scale (dB or Linear), Reference |
| **Vibration** (`UniversalVibration`) | Vibration severity as ISO 10816 measures it, from up to three acceleration axes in m/s²: each axis is high-pass filtered and integrated to velocity, and per window it outputs `VelocityRms` in mm/s (the root sum of squares of the axes), `AccelerationRms`, `Peak`, `CrestFactor`, `DominantFrequency` in Hz and `Zone`, 1 to 4 by the limits of the machine class | Window (1 s), HighPass (10 Hz), MachineClass (I small machines to IV large ones on flexible foundations) |
| **Sound level meter** (`UniversalSoundLevelMeter`) | A sound level meter after IEC 61672 on one audio input: the frequency weighting (A, C or Z) and the time weighting (Fast 125 ms or Slow 1 s) give `Level`, named as the standard names it (LAF, LCS and so on), with `Leq` since the start or the last Reset and `Max`, every 0.1 s; with bands it also outputs `Bands`, the level of each octave or third octave every second | FrequencyWeighting (A), TimeWeighting (Fast), Bands (Octaves), Reference (0 means 20 µPa when the input's unit is Pa and 1 otherwise), Reset |
| **Dashboard** (`UniversalDashboard`) | A web page of live panels, one per connected signal, served by the device for any browser on the network: a number with its line over the duration, an array such as a spectrum as a curve, or a picture. Its Settings view changes the device, with a PIN | Title, Duration (10 s), WebServer (on), WebPort (8090), Pin, WebAddress (read only) |

Levels below −160 dB, such as digital silence, read −160 dB. The sound
level meter reads true sound pressure levels only from an input in pascals,
such as a calibrated phone microphone; otherwise its levels are relative to
full scale. Its weighting filters are digital approximations that hold within
the standard's tolerances well below half the sample rate. The Dashboard's
input ports grow as the renderer's do. Its page is plain HTML and
JavaScript, served at `/`; `/data?after=N` waits up to a second for news and
answers JSON, and `/picture?panel=N` the newest picture of a panel.

The page's **Settings** view shows the device the Dashboard belongs to: its
channels, function blocks, synchronization and devices, each with its status,
and the properties of the one selected. It changes numbers, true or false,
text and selections, runs procedures without arguments, switches channels and
function blocks on and off, sets the operation mode, and adds and removes the
Dashboard's panels. Other property types are shown read-only. Every change
needs the PIN in the Pin property; without a PIN, the page only shows. After
five wrong PINs, it refuses changes for half a minute. The Dashboard's own
WebServer, WebPort and Pin stay with openDAQ clients, so the page cannot cut
itself off. The page uses plain HTTP: anyone on the network can watch, and
the PIN travels unencrypted, so use it on a network you trust.

| Route | |
|---|---|
| `GET /api/tree` | The device's components |
| `GET /api/properties?id=` | A component's properties, status, and a device's operation modes |
| `GET /api/signals` | The device's signals, for adding panels |
| `POST /api/unlock` | Checks the PIN |
| `POST /api/property`, `/api/execute`, `/api/active`, `/api/mode` | A change, as a form with `pin`, `id` and the value |
| `POST /api/panel`, `/api/panel/remove` | Adds a panel for a signal, or removes one |

## Requirements

| What | Version | Notes |
|---|---|---|
| macOS or Linux | | The build script is a POSIX shell script. macOS builds for Apple silicon |
| A C++17 compiler | | Xcode's command line tools on macOS (`xcode-select --install`), GCC or Clang on Linux |
| CMake | 3.24 or newer | `brew install cmake` on macOS |
| Ninja | | `brew install ninja` on macOS |
| git | | To clone the openDAQ SDK |
| Network access | | On the first build, for the SDK and its dependencies |
| ccache | optional | Used when installed; repeated builds are much faster |

## Building

### For the computer (macOS or Linux)

```bash
git clone https://github.com/JaKaZc/opendaq-universal-function-blocks.git
cd opendaq-universal-function-blocks
tool/build_desktop.sh
```

The script:

1. clones the openDAQ SDK into `build/openDAQ` and checks out `v3.40.3`,
2. builds and installs the SDK's core into `build/sdk-<platform>-install`,
3. builds the module against it into `dist/<platform>/`, for example
   `dist/darwin-arm64/libuniversal_function_blocks_module.module.dylib`.

The first run takes a few minutes. Later runs only rebuild what changed.
This script is for using the module on its own. openDAQ Studio
([opendaq-studio-desktop](https://github.com/JaKaZc/opendaq-studio-desktop)) does not need it: its
engine build compiles the module from these sources when this repository is
beside it, and ships it with the app.

| Variable | Default | Meaning |
|---|---|---|
| `OPENDAQ_SOURCE` | `https://github.com/openDAQ/openDAQ.git` | Where to clone the SDK from; a local clone is faster |
| `SDK_REF` | `v3.40.3` | The SDK release |
| `CMAKE`, `NINJA` | `cmake`, `ninja` | The tools, when they are not on the PATH |

To try the module in any openDAQ application of the same release, put the
library into that application's module folder, or load it from its folder:

```cpp
auto instance = daq::InstanceBuilder().addModulePath("path/to/dist/darwin-arm64").build();
auto renderer = instance.addFunctionBlock("UniversalRenderer");
```

### For Android

The Device Server's `tool/build_native.sh`
([opendaq-device-server](https://github.com/JaKaZc/opendaq-device-server)) builds the module for
Android arm64 from these sources, against its own SDK build, with the rest of
its native code. Nothing needs to be built here first. Another Android app
builds it the same way: `CMakeLists.txt` needs only an openDAQ SDK built for
Android, with the patch the Android device module carries.

### For Windows

openDAQ Studio's `engine/tool/build_windows.sh` cross-compiles the module for
Windows x64 on a Mac with `clang-cl`, against the DLLs of the openDAQ
`win_amd64` wheel. On Windows itself, build the same `CMakeLists.txt` with
Visual Studio and `CMAKE_PREFIX_PATH` set to an openDAQ SDK v3.40.3 install.
The Windows build has not been tried on a Windows computer yet.

## Layout

| Path | |
|---|---|
| `src/renderer.cpp` | The function block: properties, input ports, reading, layout in 2D and 3D, outputs |
| `src/surface.h` | What the layout draws on |
| `src/recorder.cpp` | Records the drawing as plot data |
| `src/canvas.cpp` | Draws the pixels of a picture: anti-aliased lines, text and pictures |
| `src/png.cpp` | The PNG encoder, with its own deflate, on several cores |
| `src/jpeg.cpp` | The baseline JPEG decoder, with EXIF orientation |
| `src/web.cpp` | The web server and its page |
| `src/text.cpp` | Measuring text with the built-in font |
| `src/font_data.h` | DejaVu Sans at 16 pixels: widths and glyphs, generated by `tool/make_font.py` |
| `src/derived.cpp` | The Level and Spectrum function blocks |
| `src/engineering.cpp` | The Vibration and Sound level meter function blocks |
| `src/dashboard.cpp` | The Dashboard function block and its page |
| `src/signal_reader.cpp` | Reading an input port's packets as times and values |
| `src/module.cpp` | The module |
| `fonts/` | The DejaVu fonts, and their license |
| `tool/build_desktop.sh` | The build for macOS and Linux |
| `tool/make_font.py` | Generates `src/font_data.h` from the DejaVu font |

## License

Apache License 2.0, the same as the openDAQ SDK. See [LICENSE](LICENSE). The
DejaVu fonts in `fonts/` have their own license, in
`fonts/bitstream-vera-license.txt`.

This is an independent project built on the openDAQ SDK. openDAQ is a
trademark of its respective owners.
