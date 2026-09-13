cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)

project(mapcoopa)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always CMAKE_CURRENT_SOURCE_DIR/CMAKE_CURRENT_BINARY_DIR, never
# CMAKE_SOURCE_DIR/CMAKE_BINARY_DIR: consumers pull this file in via
# add_subdirectory(), where the un-prefixed variables point at the *top-level*
# project's directories rather than mapcoopa's own. Same reasoning as
# libcoopa/CMakeLists.txt:11-18.
get_filename_component(ROOT_DIR_PARENT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

# --- Sibling library ---
# coopa/maps reaches into libcoopa for exactly two headers --
# coopa/collections/yaml_map.h (map_yaml.h) and coopa/debug/logger.h (every
# pass) -- and picks up glm and fkYAML from coopa::lib's include path on the
# way, which is why neither is vendored under includes/ here.
set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
if(NOT TARGET coopa::lib)
    add_subdirectory(${LIBCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libcoopa-build)
endif()

# Configure and include root directory header
configure_file(configuration/root_directory.h.in configuration/root_directory.h)

# --- coopa::maps: the public, header-only consumable target. ---
# mapcoopa is header-only and produces *data* -- a MapGraph, plus optional
# software PNG renders. Nothing here touches Vulkan, GLFW or any sibling repo
# other than libcoopa. Named mapcoopa_lib (not `mapcoopa`) because `cplay`
# resolves the run binary as `./build/<project name>` -- the generator
# executable below must keep the bare `mapcoopa` target name, matching
# project(mapcoopa) above.
add_library(mapcoopa_lib INTERFACE)
add_library(coopa::maps ALIAS mapcoopa_lib)
target_compile_features(mapcoopa_lib INTERFACE cxx_std_20)
target_include_directories(mapcoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}            # coopa/maps/...
    ${CMAKE_CURRENT_SOURCE_DIR}/includes   # delaunator, noise (FastNoiseLite), stb
    ${CMAKE_CURRENT_BINARY_DIR}/configuration)
target_link_libraries(mapcoopa_lib INTERFACE coopa::lib)

# --- mapcoopa's own executables/tests. Guarded so a consumer that pulls this
# repo in via add_subdirectory() gets only the coopa::maps target above -- not
# a second `mapcoopa`/`mapcoopa_tests` binary or ctest entry mixed into its own
# build. Standalone builds (CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR, i.e.
# cbuild/cplay/ctest run from this repo directly) are completely unaffected.
# Same guard sfxcoopa/CMakeLists.txt and uicoopa/CMakeLists.txt already use. ---
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # The map generator owns the bare project name so `cplay` *is* the generate
    # command: `cbuild && cplay --seed=42 --out=world`. cplay forwards its
    # arguments verbatim, so every flag in examples/map_generator.cpp works.
    add_executable(mapcoopa examples/map_generator.cpp)
    target_link_libraries(mapcoopa PRIVATE coopa::maps)

    # The test suite is therefore a separate target, unlike the sibling repos
    # where the bare name is the test runner.
    add_executable(mapcoopa_tests test.cpp)
    target_link_libraries(mapcoopa_tests PRIVATE coopa::maps)

    enable_testing()
    add_test(NAME mapcoopa_tests COMMAND mapcoopa_tests)
endif()
