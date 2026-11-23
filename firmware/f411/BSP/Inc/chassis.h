/**
  ******************************************************************************
  * @file    chassis.h
  * @brief   MG310 四驱底盘（"电机驱动例程"里那套 4 通道驱动板）的 UART 驱动
  *
  * 协议取自 project/电机驱动例程/esp32uart.ino：
  *     ASCII 帧，'$' 开头 '!' 结尾，115200 8N1
  *
  *     $MOTOR_4CH_SET:<mode>!                     0=520电机 1=TT电机 2=310电机
  *     $MOTOR_4CH_SET_ENCPDER_POLARITY:<0|1>!     注意固件里就拼成 ENCPDER
  *     $Car_Pwm:<a*100>,<b*100>,<c*100>,<d*100>!  开环 PWM（-100..100）
  *     $Car:<a>,<b>,<c>,<d>!                      闭环速度
  *     $MOTOR_4CH_READ:battery!            -> $MOTOR_4CH_Battery:<mV>!
  *     $MOTOR_4CH_READ:encoder_20ms!       -> $MOTOR_4CH_Encoder_20ms:a,b,c,d!
  *     $MOTOR_4CH_READ:encoder_total!      -> $MOTOR_4CH_Encoder_Total:a,b,c,d!
  *
  * 挂在 USART2（PA2/PA3，CubeMX 里标成 RUN_TX/RUN_RX）。
  ******************************************************************************
  */
#ifndef __CHASSIS_H
#define __CHASSIS_H

#include "main.h"
#include <stdbool.h>

#define CHASSIS_MOTOR_520    0u
#define CHASSIS_MOTOR_TT     1u
#define CHASSIS_MOTOR_MG310  2u

void     Chassis_Init(UART_HandleTypeDef *huart);          /* 设电机类型 + 停车 */

void     Chassis_SetSpeed(float a, float b, float c, float d);  /* $Car: */
void     Chassis_SetPwm(float a, float b, float c, float d);    /* $Car_Pwm:（入参 -1..1） */
void     Chassis_Stop(void);

void     Chassis_RequestBattery(void);
uint32_t Chassis_BatteryMv(void);          /* 0 = 还没读到 */
bool     Chassis_Online(void);             /* 收到过任意一条回包 */

void     Chassis_Tick1s(void);             /* 1Hz：顺手读一次电池电压 */

/* ISR 侧 */
void     Chassis_RxIsr(UART_HandleTypeDef *huart);

#endif /* __CHASSIS_H */
