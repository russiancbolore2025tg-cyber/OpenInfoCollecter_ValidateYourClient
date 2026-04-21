cmake_minimum_required(VERSION 3.10)
project(telemetry)

add_library(telemetry SHARED telemetry.c)
find_library(log-lib log)
target_link_libraries(telemetry ${log-lib})
