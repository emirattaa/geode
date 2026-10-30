cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AutoTrajectoryGeode)

find_package(Geode REQUIRED)

add_geode_mod(
    AutoTrajectoryGeode
    SOURCES
        src/main.cpp
)
