cmake_minimum_required(VERSION 3.22)
project(panvkbench C)

if(NOT MESA_ROOT)
    message(FATAL_ERROR "MESA_ROOT must point to the PanVK checkout")
endif()

add_library(panvkbench SHARED benchmark.c bc_emulation.c)
target_compile_definitions(panvkbench PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(panvkbench PRIVATE -O2 -fvisibility=default -Wall -Wextra)
target_include_directories(panvkbench PRIVATE
    "${MESA_ROOT}/work/mesa/include"
    "${MESA_ROOT}/work/mesa/src"
)
target_link_libraries(panvkbench PRIVATE android log dl m)
