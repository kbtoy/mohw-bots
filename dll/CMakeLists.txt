cmake_minimum_required(VERSION 3.20)
project(mohw_bots LANGUAGES CXX)

# lanmohw.exe is a 32-bit process, so the proxy DLL must be 32-bit too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure for 32-bit: cmake -S dll -B build -A Win32")
endif()

add_library(mohw_bots SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/log.cpp
    src/engine.cpp
    src/hook.cpp
    src/bots.cpp
    src/trace.cpp
    src/overrides.cpp
    src/guards.cpp
    src/brain.cpp
    src/nav.cpp
    src/scoring.cpp
    src/scoreboard.cpp
    src/spotting.cpp
    src/combatlog.cpp
    src/grenades.cpp
    src/limp.cpp
    src/melee.cpp
    src/hardcore.cpp
    src/rcon.cpp
    src/rconsrv.cpp
    src/modes.cpp
    src/objectives.cpp
    src/relaunch.cpp
    src/levelchange.cpp
    src/xinput1_3.def
)

# Loaded by the game as its XInput 1.3 import; the real DLL is forwarded to from proxy.cpp.
set_target_properties(mohw_bots PROPERTIES
    OUTPUT_NAME xinput1_3
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"  # static CRT: no VC++ redist needed
)
target_compile_features(mohw_bots PRIVATE cxx_std_17)
target_compile_options(mohw_bots PRIVATE /W4 /utf-8 /permissive-)
target_compile_definitions(mohw_bots PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
