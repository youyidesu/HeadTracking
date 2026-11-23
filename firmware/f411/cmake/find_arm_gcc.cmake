# =============================================================================
# find_arm_gcc.cmake —— 不依赖 PATH 地定位 arm-none-eabi-gcc
#
# 背景：cmake/gcc-arm-none-eabi.cmake 里原本写的是
#     set(CMAKE_C_COMPILER arm-none-eabi-gcc)      # 裸命令，靠 PATH
# 一旦 PATH 里没有工具链（IDE 的 build/configure 环境、清理过的终端、换台机器），
# CMake 就直接报：
#     The CMAKE_C_COMPILER: arm-none-eabi-gcc is not a full path and was not
#     found in the PATH.      <- 也就是"找不到编译器"
#
# 这个文件在 project() 之前被 include，按下面顺序找：
#   1. -DARM_GCC_PATH=<全路径>     （手工指定，优先级最高）
#   2. PATH                         （IDE 自带环境时走这条）
#   3. 常见安装位置                 （STM32Cube 扩展包的 bundle / CubeIDE 插件 /
#                                    GNU Arm Toolchain 安装目录 / Linux 包管理器）
# 找到后把它的 bin 目录塞进 PATH，并以 CACHE 条目形式写进 CMakeCache.txt
# （VS Code 的 CMake Tools 只认缓存文件里的 CMAKE_C_COMPILER，不然会弹
#   "在缓存文件中找不到编译器。 / Unable to determine the compiler in the cache file"）。
#
# 用法：在 cmake/gcc-arm-none-eabi.cmake 的 project() 之前加一行
#     include(${CMAKE_CURRENT_LIST_DIR}/find_arm_gcc.cmake OPTIONAL)
# 注意：cmake/gcc-arm-none-eabi.cmake 是 STM32CubeMX 生成的，重新转换工程时会被
#       覆盖 —— 那时把上面这一行补回去即可（本文件是新增文件，不受影响）。
# =============================================================================

set(_arm_exe "")
set(_arm_path_sep ":")
if(CMAKE_HOST_WIN32)
  set(_arm_exe ".exe")
  set(_arm_path_sep ";")
endif()
set(_arm_gcc_name "arm-none-eabi-gcc${_arm_exe}")

# 1+2. 手工指定 / PATH
if(NOT ARM_GCC_PATH)
  find_program(ARM_GCC_PATH NAMES ${_arm_gcc_name})
endif()

# 3. 常见安装位置
if(NOT ARM_GCC_PATH)
  set(_arm_gcc_globs
    # STM32Cube 扩展包（CubeMX / CubeCLT / VS Code 插件共用的 bundles 目录）
    "$ENV{LOCALAPPDATA}/stm32cube/bundles/gnu-tools-for-stm32/*/bin/${_arm_gcc_name}"
    "$ENV{USERPROFILE}/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/*/bin/${_arm_gcc_name}"
    "$ENV{HOME}/.local/share/stm32cube/bundles/gnu-tools-for-stm32/*/bin/${_arm_gcc_name}"
    "/opt/st/stm32cubeclt*/GNU-tools-for-STM32/bin/${_arm_gcc_name}"
    # STM32CubeIDE 自带工具链
    "C:/ST/STM32CubeIDE*/STM32CubeIDE/plugins/*gnu-tools*/tools/bin/${_arm_gcc_name}"
    "$ENV{ProgramFiles}/STMicroelectronics/STM32Cube/STM32CubeIDE*/STM32CubeIDE/plugins/*gnu-tools*/tools/bin/${_arm_gcc_name}"
    "C:/Program Files (x86)/STMicroelectronics/STM32Cube/STM32CubeIDE*/STM32CubeIDE/plugins/*gnu-tools*/tools/bin/${_arm_gcc_name}"
    # 官方独立安装包
    "C:/Program Files (x86)/GNU Arm Embedded Toolchain/*/bin/${_arm_gcc_name}"
    "$ENV{ProgramFiles}/GNU Arm Embedded Toolchain/*/bin/${_arm_gcc_name}"
    # Linux 发行版 / homebrew
    "/usr/bin/${_arm_gcc_name}"
    "/usr/local/bin/${_arm_gcc_name}"
    "/opt/*/bin/${_arm_gcc_name}"
  )

  set(_arm_hits "")
  foreach(_g IN LISTS _arm_gcc_globs)
    file(GLOB _found "${_g}")
    if(_found)
      list(APPEND _arm_hits ${_found})
    endif()
  endforeach()

  if(_arm_hits)
    list(SORT _arm_hits)                  # 版本号字典序升序
    list(GET _arm_hits -1 ARM_GCC_PATH)   # 取最后一个 = 版本最新的
  endif()
endif()

if(ARM_GCC_PATH)
  get_filename_component(ARM_GCC_BIN_DIR "${ARM_GCC_PATH}" DIRECTORY)
  file(TO_CMAKE_PATH "${ARM_GCC_BIN_DIR}" ARM_GCC_BIN_DIR)
  set(ENV{PATH} "${ARM_GCC_BIN_DIR}${_arm_path_sep}$ENV{PATH}")

  # ---------------------------------------------------------------------------
  # 关键：把编译器以**缓存条目**的形式写进 CMakeCache.txt。
  #
  # 工具链文件里普通 set() 出来的 CMAKE_C_COMPILER 只会落在
  #   CMakeFiles/<ver>/CMakeCCompiler.cmake
  # 里，CMakeCache.txt 中并没有这一条；而 VS Code 的 CMake Tools 是去**缓存文件**
  # 里读编译器的，读不到就弹：
  #     在缓存文件中找不到编译器。      (Unable to determine the compiler in the cache file)
  # 用 CACHE FILEPATH 写一次就有这条了，CMake Tools / clangd / IntelliSense 都能读到。
  # ---------------------------------------------------------------------------
  set(_arm_c   "${ARM_GCC_BIN_DIR}/arm-none-eabi-gcc${_arm_exe}")
  set(_arm_cxx "${ARM_GCC_BIN_DIR}/arm-none-eabi-g++${_arm_exe}")

  if((NOT DEFINED CMAKE_C_COMPILER) OR (NOT EXISTS "${CMAKE_C_COMPILER}"))
    set(CMAKE_C_COMPILER   "${_arm_c}"   CACHE FILEPATH "C compiler" FORCE)
  endif()
  if((NOT DEFINED CMAKE_ASM_COMPILER) OR (NOT EXISTS "${CMAKE_ASM_COMPILER}"))
    set(CMAKE_ASM_COMPILER "${_arm_c}"   CACHE FILEPATH "ASM compiler" FORCE)
  endif()
  if((NOT DEFINED CMAKE_CXX_COMPILER) OR (NOT EXISTS "${CMAKE_CXX_COMPILER}"))
    set(CMAKE_CXX_COMPILER "${_arm_cxx}" CACHE FILEPATH "CXX compiler" FORCE)
  endif()

  if(NOT _ARM_GCC_REPORTED)
    message(STATUS "arm-none-eabi-gcc: ${ARM_GCC_BIN_DIR}")
    set(_ARM_GCC_REPORTED 1)
  endif()
else()
  if(NOT _ARM_GCC_REPORTED)
    message(WARNING
      "没找到 arm-none-eabi-gcc。三种解法：\n"
      "  1) 把工具链 bin 目录加进 PATH；\n"
      "  2) 配置时加 -DARM_GCC_PATH=<...>/bin/arm-none-eabi-gcc(.exe)；\n"
      "  3) 在 STM32CubeMX / CubeIDE 里重新安装 GNU Tools for STM32 扩展包。")
    set(_ARM_GCC_REPORTED 1)
  endif()
endif()

unset(_arm_gcc_name)
unset(_arm_path_sep)
unset(_arm_gcc_globs)
unset(_arm_hits)
unset(_found)
unset(_g)
