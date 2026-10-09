cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(SmoothExtrapolation VERSION 1.0.0)

add_library(${PROJECT_NAME} SHARED main.cpp)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "No se encontro GEODE_SDK. Instala el SDK de Geode y define la variable de entorno.")
else()
    message(STATUS "Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
