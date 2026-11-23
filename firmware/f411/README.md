# STM32F411 云台固件

## 编译

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

工具链：`arm-none-eabi-gcc 14.3`（STM32CubeCLT 自带）

## 这个目录里为什么没有完整的 HAL / CMSIS

为了控制仓库体积，`Drivers/` 只保留了工程**实际编译**的 17 个 HAL 模块：

```
hal_cortex  hal_dma  hal_dma_ex  hal_exti
hal_flash   hal_flash_ex  hal_flash_ramfunc
hal_gpio    hal_msp
hal_pwr     hal_pwr_ex
hal_rcc     hal_rcc_ex
hal_spi
hal_tim     hal_tim_ex
hal_uart
```

`Drivers/CMSIS`（ARM 内核头 + ST 器件头）已移除。

补回来的两种方式：

1. **用 STM32CubeMX 重新生成**（推荐）——打开 `stm32test.ioc`，Generate Code，
   CubeMX 会把 `Drivers/CMSIS` 和完整 HAL 重新铺好，本目录的 `BSP/`、`Core/` 不会被覆盖。
2. **从 STM32Cube FW_F4 包拷贝**——`STM32Cube_FW_F4_V1.28.0/Drivers/` 下同名目录整体复制过来。

## 烧录

```bash
STM32_Programmer_CLI -c port=SWD -w release/f411/stm32test.elf -v -rst
```

或直接用 `release/f411/stm32test.hex`。
