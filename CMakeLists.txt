cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(OpenAudioMatrixRouter
    VERSION 0.1.0
    DESCRIPTION "Open-source cross-platform audio matrix router"
    LANGUAGES CXX)

option(OAMR_BUILD_TESTS "Build OAMR tests" ON)
option(OAMR_ENABLE_GSTREAMER "Build the GStreamer runtime backend" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(oamr_core
    src/core/audio_graph.cpp
)
add_library(oamr::core ALIAS oamr_core)

target_include_directories(oamr_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(oamr_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(oamr_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(oamr_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(OAMR_ENABLE_GSTREAMER)
    find_package(GStreamer 1.20 REQUIRED COMPONENTS app audio rtp)

    add_library(oamr_gstreamer
        src/gstreamer/device_enumerator.cpp
        src/gstreamer/local_router_engine.cpp
        src/gstreamer/rtp_opus_pipeline.cpp
        src/web/web_server.cpp
    )
    add_library(oamr::gstreamer ALIAS oamr_gstreamer)
    target_link_libraries(oamr_gstreamer PUBLIC oamr::core GStreamer::gstreamer GStreamer::app GStreamer::audio GStreamer::rtp)
    target_include_directories(oamr_gstreamer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_compile_features(oamr_gstreamer PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(oamr_gstreamer PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_link_libraries(oamr_gstreamer PRIVATE ws2_32)
    endif()

    add_executable(oamr apps/oamr/main.cpp)
    target_link_libraries(oamr PRIVATE oamr::gstreamer)

    if(WIN32 AND DEFINED GSTREAMER_RUNTIME_ROOT)
        configure_file(apps/oamr/run-oamr.cmd.in "${CMAKE_CURRENT_BINARY_DIR}/run-oamr.cmd" @ONLY)
    endif()
else()
    message(STATUS "OAMR_ENABLE_GSTREAMER=OFF: only the platform-neutral core will be built")
endif()

if(OAMR_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(oamr_core_tests tests/audio_graph_tests.cpp)
        target_link_libraries(oamr_core_tests PRIVATE oamr::core)
        add_test(NAME oamr_core_tests COMMAND oamr_core_tests)
    endif()
endif()

install(TARGETS oamr_core
    EXPORT OAMRTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(OAMR_ENABLE_GSTREAMER)
    install(TARGETS oamr_gstreamer oamr
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
