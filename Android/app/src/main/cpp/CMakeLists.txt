cmake_minimum_required(VERSION 3.22.1)
project(oamr_audio LANGUAGES C CXX)

include(FetchContent)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)

add_library(oamr_audio SHARED oamr_audio.cpp)
find_package(oboe CONFIG REQUIRED)
target_compile_features(oamr_audio PRIVATE cxx_std_20)
target_link_libraries(oamr_audio PRIVATE oboe::oboe opus android log)
