# Finds GStreamer 1.x. Unix-like systems use pkg-config; Windows can use the
# official MSVC installer directly through GSTREAMER_ROOT or its default path.
include(FindPackageHandleStandardArgs)

set(_gst_components ${GStreamer_FIND_COMPONENTS})
if(NOT _gst_components)
    set(_gst_components app audio rtp)
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET gstreamer-1.0>=1.20)
endif()

if(GSTREAMER_FOUND)
    add_library(GStreamer::gstreamer ALIAS PkgConfig::GSTREAMER)
    set(GStreamer_VERSION "${GSTREAMER_VERSION}")
    if(WIN32 AND DEFINED GSTREAMER_ROOT AND EXISTS "${GSTREAMER_ROOT}/bin/gstreamer-1.0-0.dll")
        set(GSTREAMER_RUNTIME_ROOT "${GSTREAMER_ROOT}")
    endif()
    foreach(_component IN LISTS _gst_components)
        pkg_check_modules(GSTREAMER_${_component} REQUIRED IMPORTED_TARGET gstreamer-${_component}-1.0>=1.20)
        add_library(GStreamer::${_component} ALIAS PkgConfig::GSTREAMER_${_component})
    endforeach()
    set(GStreamer_FOUND TRUE)
else()
    # The official Windows installers default to this directory. Allow callers
    # to override it for private SDK installs: -DGSTREAMER_ROOT=C:/path/to/sdk
    set(GSTREAMER_ROOT "$ENV{GSTREAMER_ROOT_X86_64}" CACHE PATH "GStreamer MSVC installation root")
    if(NOT GSTREAMER_ROOT AND WIN32)
        set(GSTREAMER_ROOT "C:/Program Files/GStreamer/1.0/msvc_x86_64")
    endif()

    find_path(GSTREAMER_INCLUDE_DIR gst/gst.h
        HINTS "${GSTREAMER_ROOT}/include/gstreamer-1.0")
    find_library(GSTREAMER_LIBRARY NAMES gstreamer-1.0
        HINTS "${GSTREAMER_ROOT}/lib")
    find_library(GLIB_LIBRARY NAMES glib-2.0 HINTS "${GSTREAMER_ROOT}/lib")
    find_library(GOBJECT_LIBRARY NAMES gobject-2.0 HINTS "${GSTREAMER_ROOT}/lib")
    find_library(GIO_LIBRARY NAMES gio-2.0 HINTS "${GSTREAMER_ROOT}/lib")
    find_path(GLIB_INCLUDE_DIR glib.h HINTS "${GSTREAMER_ROOT}/include/glib-2.0")
    find_path(GLIB_CONFIG_INCLUDE_DIR glibconfig.h HINTS "${GSTREAMER_ROOT}/lib/glib-2.0/include")

    find_package_handle_standard_args(GStreamer
        REQUIRED_VARS GSTREAMER_INCLUDE_DIR GSTREAMER_LIBRARY GLIB_LIBRARY GOBJECT_LIBRARY GIO_LIBRARY GLIB_INCLUDE_DIR GLIB_CONFIG_INCLUDE_DIR)

    if(GStreamer_FOUND)
        set(GSTREAMER_RUNTIME_ROOT "${GSTREAMER_ROOT}")
        find_file(GSTREAMER_RUNTIME NAMES gstreamer-1.0-0.dll HINTS "${GSTREAMER_ROOT}/bin" REQUIRED)
        add_library(GStreamer::gstreamer SHARED IMPORTED)
        set_target_properties(GStreamer::gstreamer PROPERTIES
            IMPORTED_IMPLIB "${GSTREAMER_LIBRARY}"
            IMPORTED_LOCATION "${GSTREAMER_RUNTIME}"
            INTERFACE_INCLUDE_DIRECTORIES "${GSTREAMER_INCLUDE_DIR};${GLIB_INCLUDE_DIR};${GLIB_CONFIG_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${GLIB_LIBRARY};${GOBJECT_LIBRARY};${GIO_LIBRARY}")
        foreach(_component IN LISTS _gst_components)
            find_library(GSTREAMER_${_component}_LIBRARY NAMES gst${_component}-1.0
                HINTS "${GSTREAMER_ROOT}/lib" REQUIRED)
            set(_gst_component_dll "gst${_component}-1.0-0.dll")
            find_file(GSTREAMER_${_component}_RUNTIME NAMES "${_gst_component_dll}" HINTS "${GSTREAMER_ROOT}/bin" REQUIRED)
            add_library(GStreamer::${_component} SHARED IMPORTED)
            set_target_properties(GStreamer::${_component} PROPERTIES
                IMPORTED_IMPLIB "${GSTREAMER_${_component}_LIBRARY}"
                IMPORTED_LOCATION "${GSTREAMER_${_component}_RUNTIME}")
        endforeach()
    endif()
endif()
