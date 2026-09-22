cmake_minimum_required(VERSION 3.24)
project(ResMon VERSION 1.7.4 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

foreach(sensor_file
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/PawnIO_setup.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/IntelMSR.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/AMDFamily17.bin")
    if(NOT EXISTS "${sensor_file}")
        message(FATAL_ERROR "Missing ${sensor_file}. Run FETCH_SENSOR_DEPS.ps1 or BUILD_AND_RUN.cmd first.")
    endif()
endforeach()

configure_file(assets/resmon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resmon.rc @ONLY)

add_executable(ResMon WIN32
    src/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/resmon.rc
)

target_compile_definitions(ResMon PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(ResMon PRIVATE
    d2d1
    dwrite
    dwmapi
    pdh
    powrprof
    shlwapi
    advapi32
    comctl32
    shell32
    windowscodecs
    ole32
)

if(MSVC)
    target_compile_options(ResMon PRIVATE /W4 /permissive- /EHsc /O2)
    target_link_options(ResMon PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

set_target_properties(ResMon PROPERTIES
    OUTPUT_NAME "ResMon"
)

install(TARGETS ResMon RUNTIME DESTINATION .)
