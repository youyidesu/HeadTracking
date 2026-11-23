cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME stm32test)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    # Core/Src 里的 melody_*.h 是被 main.c 直接 #include 的数据头文件，不用加进来

    # ---- 云台 BSP（2026-11-14 起） ----
    # zdt.c / scurve.c 是纯逻辑，PC 上也能编（tools/host_test 里跑单测）
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/zdt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/scurve.c
    # zdt_port.c 整体包在 #ifdef HAL_UART_MODULE_ENABLED 里，UART 没在 CubeMX
    # 里启用时它会编译成空文件，不会造成链接错误
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/zdt_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/app_gimbal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/chassis.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/esp_link.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
