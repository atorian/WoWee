# Configure this directory separately with an x86 toolchain for 32-bit adapters:
# cmake -S tools/server_extension_host -B build-extension-x86 -A Win32
cmake_minimum_required(VERSION 3.15)
project(wowee_extension_host LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(wowee_extension_host main.cpp)
target_include_directories(wowee_extension_host PRIVATE ../../include)
target_link_libraries(wowee_extension_host PRIVATE ${CMAKE_DL_LIBS})
if(MSVC)
    set_property(TARGET wowee_extension_host PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(WOWEE_EXTENSION_HOST_BUILD_FIXTURE "Build the test adapter for this architecture" OFF)
if(WOWEE_EXTENSION_HOST_BUILD_FIXTURE)
    add_library(server_extension_fixture SHARED ../../tests/server_extension_fixture.cpp)
    target_include_directories(server_extension_fixture PRIVATE ../../include)
    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_sources(server_extension_fixture PRIVATE ../../tests/server_extension_fixture_x86.def)
    endif()
endif()

if(WIN32)
    target_link_libraries(wowee_extension_host PRIVATE shell32)
endif()
