/**
  ******************************************************************************
  * @file    bsp_uart.c
  * @brief   UART 中断收的统一分发
  *
  * 三个 USART 只共用一个 HAL_UART_RxCpltCallback：先问 ZDT（它按句柄认领），
  * 不是 ZDT 的再给底盘。C5 那条走 DMA+IDLE，在 esp_link.c 里自己处理。
  ******************************************************************************
  */
#include "main.h"
#include "zdt_port.h"
#include "chassis.h"

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (zdt_port_on_rx(huart)) return;

  Chassis_RxIsr(huart);          /* 内部比对句柄，不匹配直接返回 */
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  zdt_port_on_error(huart);
  __HAL_UART_CLEAR_OREFLAG(huart);
}
