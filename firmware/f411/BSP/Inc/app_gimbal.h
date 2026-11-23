/**
  ******************************************************************************
  * @file    app_gimbal.h
  * @brief   云台应用层：两轴 ZDT 步进 + S 曲线 + 演示动作
  *
  * 串口分配（按当前 CubeMX 配置）：
  *     USART6 (PA11/PA12, YAW_TX/RX)   -> YAW   驱动器，地址 1
  *     USART1 (PA9 /PA10 , PITCH_TX/RX)-> PITCH 驱动器，地址 2
  *     USART2 (PA2 /PA3 , RUN_TX/RX)   -> 底盘（本文件暂不接管）
  *
  * 主循环节拍：Tick1ms() 收包，Tick5ms() 跑 200Hz 运动更新。
  ******************************************************************************
  */
#ifndef __APP_GIMBAL_H
#define __APP_GIMBAL_H

#include "main.h"
#include "zdt_port.h"
#include "scurve.h"

/* CubeMX 在 main.c 里定义的句柄，头文件里不重复声明，这里 extern 引进来 */
extern UART_HandleTypeDef huart1;   /* PITCH */
extern UART_HandleTypeDef huart2;   /* RUN / 底盘 */
extern UART_HandleTypeDef huart6;   /* YAW   */

void    Gimbal_Init(void);       /* 上电调用一次：初始化两轴、使能、清零 */
void    Gimbal_Tick1ms(void);    /* 1 kHz：串口收包 */
void    Gimbal_Tick5ms(void);    /* 200 Hz：S 曲线推进 + 下发位置 */
uint8_t Gimbal_DemoDone(void);   /* 演示/停靠完成返回 1 */
void    Gimbal_Park(void);        /* 随时回停靠角（HOME_YAW_DEG / HOME_PITCH_DEG），并锁住 */
void    Gimbal_Track(void);       /* 解除停靠锁存，重新跟随头瞄 */
uint8_t Gimbal_Mode(void);       /* 0=DEMO 1=TRACK 2=FAULT（给 C5 回读用） */
void    Gimbal_Diag(float *yaw_deg, float *pitch_deg,
                    uint16_t *yaw_ack, uint16_t *pitch_ack,
                    uint16_t *yaw_err, uint16_t *pitch_err,
                    uint8_t *yaw_st, uint8_t *pitch_st,
                    uint16_t *yaw_tx, uint16_t *pitch_tx);

#endif /* __APP_GIMBAL_H */
