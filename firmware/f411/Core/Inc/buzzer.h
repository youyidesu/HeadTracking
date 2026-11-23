/**
  ******************************************************************************
  * @file    buzzer.h
  * @brief   无源蜂鸣器 PWM 发声 + 非阻塞旋律播放器
  *
  * 硬件：PA6 = TIM3_CH1（AF2）。TIM3 时钟 = 100 MHz（APB1=50MHz x2），
  *       PSC 压到 1 MHz 计数，ARR = 1000000/f - 1。
  *
  * 为什么不用 HAL_Delay：
  *   整首歌 176 s，如果逐音 HAL_Delay，主循环会被完全堵死，按键没反应。
  *   这里用 HAL_GetTick()（SysTick 1 ms）做状态机，Melody_Poll() 放在
  *   while(1) 里反复调用即可，CPU 全程空闲。
  ******************************************************************************
  */
#ifndef __BUZZER_H
#define __BUZZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "main.h"

/* ---- 引脚/通道：换接线只改这三行（PB4 也是 TIM3_CH1） ---- */
#ifndef BUZZER_PORT
#define BUZZER_PORT        GPIOA
#define BUZZER_PIN         GPIO_PIN_6
#define BUZZER_PIN_AF      GPIO_AF2_TIM3
#define BUZZER_TIM_CHANNEL TIM_CHANNEL_1
#endif

/* ---- 一首曲子：频率表 + 时值表 + 长度 + 是否循环 ---- */
typedef struct
{
  const uint16_t *freq_hz;   /* 音高表，0 = 休止 */
  const uint16_t *dur_ms;    /* 时值表，单位 ms */
  uint16_t        count;     /* 音符个数 */
  uint8_t         loop;      /* 1 = 播完从头再来 */
} Melody_t;

/* ---- 基础发声 ---- */
void Buzzer_Init(void);                             /* 引脚复用 + 启动 PWM */
void Buzzer_Tone(uint16_t freq_hz);                 /* 设频，0 或 <20 则静音 */
void Buzzer_Silence(void);                          /* 占空比 0，引脚保持低电平 */
void Buzzer_Beep(uint16_t freq_hz, uint32_t ms);    /* 阻塞式短哔，按键提示用 */

/* ---- 非阻塞播放器：Poll 要放在主循环里 ---- */
void     Melody_Play(const Melody_t *m);            /* 从头开始播 */
void     Melody_Stop(void);
void     Melody_Poll(void);                         /* 主循环里高频调用 */
uint8_t  Melody_IsPlaying(void);
uint16_t Melody_Progress(void);                     /* 当前播到第几个音 */

#ifdef __cplusplus
}
#endif

#endif /* __BUZZER_H */
