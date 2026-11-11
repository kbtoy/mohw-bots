cmake_minimum_required(VERSION 3.20)
project(smoketest LANGUAGES CXX)
add_executable(smoketest main.cpp)

# Fixed at 0x400000 like the game, small enough to leave 0x500000+ free for the copied game image.
add_executable(imagetest imagetest.cpp)
target_link_options(imagetest PRIVATE /BASE:0x400000 /FIXED /DYNAMICBASE:NO)

# Offline test of the bot navigation graph: builds dll/src/nav.cpp with stub logging.
add_executable(navtest navtest.cpp ../../dll/src/nav.cpp)
target_compile_features(navtest PRIVATE cxx_std_17)
target_compile_definitions(navtest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# The RCON server with made-up data and no game: a copy of ProCon can connect to it (see dll/src/rconsrv.h).
add_executable(rcontest rcontest.cpp ../../dll/src/rconsrv.cpp)
target_compile_features(rcontest PRIVATE cxx_std_17)
target_compile_definitions(rcontest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rcontest PRIVATE /W4 /utf-8 /permissive-)

# Offline test of the map rotation ([launch] Map with modes): builds dll/src/modes.cpp with stub logging.
add_executable(modestest modestest.cpp ../../dll/src/modes.cpp)
target_compile_features(modestest PRIVATE cxx_std_17)
target_compile_definitions(modestest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(modestest PRIVATE /W4 /utf-8 /permissive-)

# Offline test of the restart for ProCon's Next Round: builds dll/src/relaunch.cpp with stub logging.
add_executable(relaunchtest relaunchtest.cpp ../../dll/src/relaunch.cpp)
target_compile_features(relaunchtest PRIVATE cxx_std_17)
target_compile_definitions(relaunchtest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(relaunchtest PRIVATE /W4 /utf-8 /permissive-)
