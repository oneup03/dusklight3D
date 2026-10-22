cmake_minimum_required(VERSION 3.16)
project(frame3d LANGUAGES C CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FRAME3D_TOP_LEVEL ON)
else ()
    set(FRAME3D_TOP_LEVEL OFF)
endif ()
option(FRAME3D_BUILD_EXAMPLES "Build the Frame-3D-Lib examples" ${FRAME3D_TOP_LEVEL})

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FRAME3D_SUPPORTED ON)
else ()
    set(FRAME3D_SUPPORTED OFF)
endif ()

if (FRAME3D_SUPPORTED)
    add_library(frame3d STATIC
            src/common.hpp
            src/frame3d.cpp
            src/gbm_allocator.cpp
            src/gbm_allocator.hpp
            src/openvr_runtime.cpp
            src/openvr_runtime.hpp
            src/screen_follower.cpp
            src/screen_follower.hpp)
    # Header-only use of openvr.h: libopenvr_api.so and libgbm are dlopen'd at runtime.
    target_include_directories(frame3d PRIVATE third_party/openvr)
    find_package(Threads REQUIRED)
    target_link_libraries(frame3d PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
else ()
    add_library(frame3d STATIC src/frame3d_stub.cpp)
endif ()
add_library(frame3d::frame3d ALIAS frame3d)
target_include_directories(frame3d PUBLIC include)
target_compile_features(frame3d PUBLIC c_std_99 PRIVATE cxx_std_17)
target_compile_definitions(frame3d PUBLIC FRAME3D_SUPPORTED=$<BOOL:${FRAME3D_SUPPORTED}>)
set_target_properties(frame3d PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (FRAME3D_BUILD_EXAMPLES AND FRAME3D_SUPPORTED)
    add_executable(frame3d-test-pattern examples/test_pattern/test_pattern.cpp)
    target_link_libraries(frame3d-test-pattern PRIVATE frame3d::frame3d ${CMAKE_DL_LIBS})
    target_compile_features(frame3d-test-pattern PRIVATE cxx_std_17)
endif ()
