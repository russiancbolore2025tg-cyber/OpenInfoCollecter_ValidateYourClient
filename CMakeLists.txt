cmake_minimum_required(VERSION 3.10)
project(telemetry)

add_library(telemetry SHARED telemetry.c)
find_library(log-lib log)

target_link_libraries(telemetry ${log-lib} dl)

# URL задаётся при сборке: cmake -DTELEMETRY_URL="https://..."
if(DEFINED TELEMETRY_URL)
    target_compile_definitions(telemetry PRIVATE TELEMETRY_URL="${TELEMETRY_URL}")
endif()
