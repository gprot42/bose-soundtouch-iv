cmake_minimum_required(VERSION 3.16)
project(vlc-upnp-renderer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# VLC installation (override with -DVLC_DIR=...)
if(NOT VLC_DIR)
    if(APPLE)
        set(VLC_DIR "/Applications/VLC.app/Contents/MacOS")
    else()
        set(VLC_DIR "/usr")
    endif()
endif()

set(VLC_SRC_DIR "${CMAKE_SOURCE_DIR}/../.vlc-src" CACHE PATH "VLC source tree for headers")
if(NOT EXISTS "${VLC_SRC_DIR}/include/vlc_common.h")
    set(VLC_SRC_DIR "/tmp/vlc-3.0.23")
endif()

set(VLC_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/cmake"
    "${VLC_SRC_DIR}/include"
    "${VLC_DIR}/include"
)

if(APPLE)
    set(VLC_LIB_DIR "${VLC_DIR}/lib")
    set(VLC_PLUGIN_DIR "${VLC_DIR}/plugins" CACHE PATH "VLC plugins install dir")
    set(VLC_CORE_LIB "${VLC_LIB_DIR}/libvlccore.dylib")
    set(MODULE_SUFFIX ".dylib")
else()
    set(VLC_LIB_DIR "${VLC_DIR}/lib")
    set(VLC_PLUGIN_DIR "${VLC_DIR}/lib/vlc/plugins" CACHE PATH "VLC plugins install dir")
    set(VLC_CORE_LIB "${VLC_LIB_DIR}/libvlccore.so")
    set(MODULE_SUFFIX ".so")
endif()

set(UPNP_COMMON_SOURCES
    src/upnp_common.c
    src/upnp_ssdp.c
    src/upnp_device.c
    src/upnp_soap.c
    src/upnp_http_serve.c
)

function(add_vlc_plugin target)
    add_library(${target} MODULE ${ARGN})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${VLC_INCLUDE_DIRS}
    )
    target_compile_definitions(${target} PRIVATE HAVE_CONFIG_H __PLUGIN__)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    set_target_properties(${target} PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "${target}_plugin"
        SUFFIX "${MODULE_SUFFIX}"
        C_VISIBILITY_PRESET default
    )
    if(APPLE)
        target_link_options(${target} PRIVATE
            "LINKER:-undefined,dynamic_lookup"
        )
    endif()
    target_link_libraries(${target} PRIVATE ${VLC_CORE_LIB} pthread)
endfunction()

set(UPNP_RENDERER_SOURCES src/renderer_discovery.c ${UPNP_COMMON_SOURCES})
if(APPLE)
    list(APPEND UPNP_RENDERER_SOURCES src/rd_macos_ui.m)
endif()

add_vlc_plugin(upnp_renderer ${UPNP_RENDERER_SOURCES})
target_compile_definitions(upnp_renderer PRIVATE MODULE_STRING=\"upnp_renderer\")
if(APPLE)
    target_link_options(upnp_renderer PRIVATE "LINKER:-framework,CoreFoundation")
endif()

add_vlc_plugin(upnp_cast
    src/stream_out_upnp.c
    src/upnp_cast.c
    ${UPNP_COMMON_SOURCES}
)
target_compile_definitions(upnp_cast PRIVATE MODULE_STRING=\"upnp_cast\")

install(TARGETS upnp_renderer upnp_cast
    LIBRARY DESTINATION "${VLC_PLUGIN_DIR}"
)

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_ssdp tests/test_ssdp.c src/upnp_ssdp.c src/upnp_common.c)
    target_include_directories(test_ssdp PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME ssdp_parse COMMAND test_ssdp)

    add_executable(test_device_parse tests/test_device_parse.c
        src/upnp_device.c src/upnp_common.c)
    target_include_directories(test_device_parse PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME device_parse COMMAND test_device_parse
        "${CMAKE_SOURCE_DIR}/fixtures/bose_device_description.xml")

    add_executable(test_soap tests/test_soap.c src/upnp_soap.c src/upnp_common.c)
    target_include_directories(test_soap PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME soap_build COMMAND test_soap)

    add_executable(test_volume tests/test_volume.c src/upnp_cast.c ${UPNP_COMMON_SOURCES})
    target_include_directories(test_volume PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME volume_map COMMAND test_volume)
endif()

message(STATUS "VLC_DIR:         ${VLC_DIR}")
message(STATUS "VLC_SRC_DIR:     ${VLC_SRC_DIR}")
message(STATUS "VLC_PLUGIN_DIR:  ${VLC_PLUGIN_DIR}")