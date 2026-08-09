cmake_minimum_required(VERSION 3.22.1)
project(oamr_audio LANGUAGES CXX)

add_library(oamr_audio SHARED oamr_audio.cpp)
find_package(oboe CONFIG REQUIRED)
target_compile_features(oamr_audio PRIVATE cxx_std_20)
target_link_libraries(oamr_audio PRIVATE oboe::oboe android log)
