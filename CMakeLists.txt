cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(OpenAudioMatrixRouter
    VERSION 0.3.0
    DESCRIPTION "Open-source cross-platform audio matrix router"
    LANGUAGES CXX)

option(OAMR_BUILD_TESTS "Build OAMR tests" ON)
option(OAMR_ENABLE_GSTREAMER "Build the GStreamer runtime backend" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# oamr_core: platform-neutral graph model. No audio, socket, or UI dependency.
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# oamr_pairing: LAN pairing control plane. No GStreamer dependency.
# ---------------------------------------------------------------------------
add_library(oamr_pairing
    src/pairing/pairing_service.cpp
)
add_library(oamr::pairing ALIAS oamr_pairing)

target_include_directories(oamr_pairing
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(oamr_pairing PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(oamr_pairing PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(oamr_pairing PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(oamr_pairing PRIVATE ws2_32)
endif()

if(OAMR_ENABLE_GSTREAMER)
    # -----------------------------------------------------------------------
    # oamr_gstreamer: concrete AudioBackend/AudioRoute implementation.
    # Public consumers only include oamr/audio/*; the classes in src/gstreamer
    # are internal implementation details and must not leak upward.
    # -----------------------------------------------------------------------
    find_package(GStreamer 1.20 REQUIRED COMPONENTS audio rtp)

    add_library(oamr_gstreamer
        src/gstreamer/device_enumerator.cpp
        src/gstreamer/rtp_opus_pipeline.cpp
        src/gstreamer/audio_backend.cpp
    )
    add_library(oamr::gstreamer ALIAS oamr_gstreamer)

    target_link_libraries(oamr_gstreamer
        PUBLIC oamr::core GStreamer::gstreamer GStreamer::audio GStreamer::rtp)
    target_include_directories(oamr_gstreamer
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gstreamer)
    target_compile_features(oamr_gstreamer PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(oamr_gstreamer PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(oamr_gstreamer PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # -----------------------------------------------------------------------
    # oamr_web: local-only web UI. Depends on the pairing service and the
    # backend factory, never on backend-internal headers.
    # -----------------------------------------------------------------------
    add_library(oamr_web
        src/web/web_server.cpp
    )
    add_library(oamr::web ALIAS oamr_web)

    target_link_libraries(oamr_web PRIVATE oamr::pairing oamr::gstreamer)
    target_include_directories(oamr_web
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(oamr_web PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(oamr_web PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(oamr_web PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32)
        target_link_libraries(oamr_web PRIVATE ws2_32)
    endif()

    # -----------------------------------------------------------------------
    # oamr: command-line entry point.
    # -----------------------------------------------------------------------
    add_executable(oamr apps/oamr/main.cpp)
    target_link_libraries(oamr PRIVATE oamr::web oamr::pairing oamr::gstreamer)

    # The browser UI is compiled by Vite to web/dist.  Keep the runtime
    # dependency-free: oamr.exe serves these files itself, and Node.js is only
    # required when a developer builds the frontend.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html")
        add_custom_target(oamr_web_assets ALL
            COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:oamr>/web"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/web/dist" "$<TARGET_FILE_DIR:oamr>/web"
            DEPENDS oamr
            COMMENT "Copying Vite Web UI next to oamr.exe")
    else()
        message(WARNING "Vite UI has not been built. Run: cd web && npm ci && npm run build")
    endif()

    if(MSVC)
        target_compile_options(oamr PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(oamr PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32 AND DEFINED GSTREAMER_RUNTIME_ROOT)
        configure_file(apps/oamr/run-oamr.cmd.in "${CMAKE_CURRENT_BINARY_DIR}/run-oamr.cmd" @ONLY)
    endif()
else()
    message(STATUS "OAMR_ENABLE_GSTREAMER=OFF: only platform-neutral core and pairing will be built")
endif()

if(OAMR_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(oamr_core_tests tests/audio_graph_tests.cpp)
        target_link_libraries(oamr_core_tests PRIVATE oamr::core)
        add_test(NAME oamr_core_tests COMMAND oamr_core_tests)
        if(OAMR_ENABLE_GSTREAMER)
            add_executable(oamr_gstreamer_smoke_tests tests/gstreamer_smoke_tests.cpp)
            target_link_libraries(oamr_gstreamer_smoke_tests PRIVATE oamr::gstreamer)
            add_test(NAME oamr_gstreamer_smoke_tests COMMAND oamr_gstreamer_smoke_tests)
            if(WIN32 AND DEFINED GSTREAMER_RUNTIME_ROOT)
                # CTest bypasses run-oamr.cmd, so provide the same runtime
                # loader and plugin paths used by the portable package.
                set_tests_properties(oamr_gstreamer_smoke_tests PROPERTIES ENVIRONMENT
                    "PATH=${GSTREAMER_RUNTIME_ROOT}/bin\;$ENV{PATH};GST_PLUGIN_PATH_1_0=${GSTREAMER_RUNTIME_ROOT}/lib/gstreamer-1.0;GST_PLUGIN_SYSTEM_PATH_1_0=${GSTREAMER_RUNTIME_ROOT}/lib/gstreamer-1.0;GST_PLUGIN_SCANNER=${GSTREAMER_RUNTIME_ROOT}/libexec/gstreamer-1.0/gst-plugin-scanner.exe")
            endif()
        endif()
    endif()
endif()

install(TARGETS oamr_core oamr_pairing
    EXPORT OAMRTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(OAMR_ENABLE_GSTREAMER)
    install(TARGETS oamr_gstreamer oamr_web oamr
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
