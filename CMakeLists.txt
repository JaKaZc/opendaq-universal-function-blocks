cmake_minimum_required(VERSION 3.24)
project(universal_function_blocks_module LANGUAGES CXX)

# An openDAQ module with the Universal renderer function block. It needs only
# the openDAQ SDK: its plot data, drawing, font, PNG encoder, JPEG decoder and
# web server are its own, so it builds the same for Android, macOS, Linux and
# Windows.
find_package(openDAQ REQUIRED)

add_library(universal_function_blocks_module SHARED
    src/module.cpp
    src/renderer.cpp
    src/recorder.cpp
    src/tape.cpp
    src/canvas.cpp
    src/png.cpp
    src/jpeg.cpp
    src/web.cpp
    src/text.cpp
    src/signal_reader.cpp
    src/derived.cpp
    src/dashboard.cpp
    src/engineering.cpp
)
target_compile_features(universal_function_blocks_module PRIVATE cxx_std_17)
target_link_libraries(universal_function_blocks_module PRIVATE daq::opendaq)
# The definitions openDAQ's own modules are built with.
target_compile_definitions(universal_function_blocks_module PRIVATE
    BUILDING_SHARED_LIBRARY
    OPENDAQ_MODULE_EXPORTS
    OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT
)
set_target_properties(universal_function_blocks_module PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    PREFIX "lib"
    SUFFIX ".module${CMAKE_SHARED_LIBRARY_SUFFIX}"
)
if(WIN32)
    # Named as the SDK's own Windows modules are, without "lib"; the web
    # server needs Winsock and the interface list.
    set_target_properties(universal_function_blocks_module PROPERTIES PREFIX "")
    target_link_libraries(universal_function_blocks_module PRIVATE ws2_32 iphlpapi)
    # windows.h would otherwise define min and max as macros.
    target_compile_definitions(universal_function_blocks_module PRIVATE NOMINMAX)
endif()
if(APPLE)
    # Loaded beside the SDK it was built for: from the SDK's modules folder,
    # whose parent holds the openDAQ libraries.
    set_target_properties(universal_function_blocks_module PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "@loader_path/..;@loader_path"
    )
endif()
