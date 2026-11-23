/**
  ******************************************************************************
  * @file    chassis.c
  * @brief   MG310 四驱底盘 UART 驱动（ASCII $...! 协议）
  *
  * 不用 printf("%f")：STM32 默认的 newlib-nano 不支持 %f，
  * 要额外加 -u _printf_float 并多吃 7KB Flash。这里自己把 float 拆成
  * "整数部分.两位小数" 拼字符串，零依赖。
  ******************************************************************************
  */
#include "chassis.h"
#include <string.h>

#define RX_LINE_MAX  64

static UART_HandleTypeDef *s_h;
static uint8_t  s_one;
static char     s_line[RX_LINE_MAX];
static uint16_t s_len;

static volatile uint32_t s_batt_mv;
static volatile bool     s_online;
static volatile uint8_t  s_enc20[4];

/* -------------------------------------------------------------------------- */
/* 小工具：不带 printf 的数字->字符串                                           */
/* -------------------------------------------------------------------------- */
static char *put_str(char *p, const char *s)
{
  while (*s) *p++ = *s++;
  return p;
}

static char *put_u32(char *p, uint32_t v)
{
  char tmp[10];
  int  n = 0;

  if (v == 0u) { *p++ = '0'; return p; }
  while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
  while (n) *p++ = tmp[--n];
  return p;
}

/* -12.34 这样的定点两位小数 */
static char *put_f2(char *p, float v)
{
  uint32_t scaled;
  int      neg = 0;

  if (v < 0.0f) { neg = 1; v = -v; }
  if (v > 10000.0f) v = 10000.0f;

  scaled = (uint32_t)(v * 100.0f + 0.5f);
  if (neg) *p++ = '-';
  p = put_u32(p, scaled / 100u);
  *p++ = '.';
  *p++ = (char)('0' + (scaled / 10u) % 10u);
  *p++ = (char)('0' + scaled % 10u);
  return p;
}

static void send(const char *buf, uint16_t n)
{
  if (s_h) HAL_UART_Transmit(s_h, (uint8_t *)buf, n, 10);
}

/* -------------------------------------------------------------------------- */
/* 解析回包                                                                    */
/* -------------------------------------------------------------------------- */
static void parse_line(const char *l)
{
  s_online = true;

  if (strncmp(l, "MOTOR_4CH_Battery:", 18) == 0)
  {
    uint32_t v = 0;
    const char *p = l + 18;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
    s_batt_mv = v;
    return;
  }

  /* Encoder_20ms / Encoder_Total 都是 "名字:a,b,c,d" */
  const char *p = strchr(l, ':');
  if (p && (strncmp(l, "MOTOR_4CH_Encoder_20ms", 22) == 0))
  {
    p++;
    for (uint8_t i = 0; i < 4u && *p; i++)
    {
      int32_t v = 0;
      int     neg = 0;
      if (*p == '-') { neg = 1; p++; }
      while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
      s_enc20[i] = (uint8_t)(neg ? -v : v);      /* 这里只存低位，够看 */
      if (*p == ',') p++;
    }
  }
}

void Chassis_RxIsr(UART_HandleTypeDef *huart)
{
  if (huart != s_h) return;

  if (s_one == '$') s_len = 0;                       /* 帧头，重新开始 */
  else if (s_one == '!')
  {
    s_line[s_len] = '\0';
    if (s_len) parse_line(s_line);
    s_len = 0;
  }
  else if (s_len < (RX_LINE_MAX - 1u)) s_line[s_len++] = (char)s_one;

  HAL_UART_Receive_IT(huart, &s_one, 1);
}

/* -------------------------------------------------------------------------- */
/* 对外                                                                       */
/* -------------------------------------------------------------------------- */
void Chassis_Init(UART_HandleTypeDef *huart)
{
  char b[48];
  char *p;

  s_h    = huart;
  s_len  = 0;
  s_batt_mv = 0;
  s_online  = false;

  /* 1) 设电机类型 = MG310（例程里 motor_mode=2 就是 310 电机） */
  p = b;
  p = put_str(p, "$MOTOR_4CH_SET:");
  p = put_u32(p, CHASSIS_MOTOR_MG310);
  *p++ = '!';
  send(b, (uint16_t)(p - b));
  HAL_Delay(100);

  /* 2) 闭环速度 0 = 先停车，避免上电乱跑 */
  Chassis_Stop();
  HAL_Delay(50);

  /* 3) 打开接收中断（之后由 HAL_UART_RxCpltCallback -> Chassis_RxIsr） */
  HAL_UART_Receive_IT(huart, &s_one, 1);
}

void Chassis_SetSpeed(float a, float b, float c, float d)
{
  char buf[64];
  char *p = buf;

  p = put_str(p, "$Car:");
  p = put_f2(p, a); *p++ = ',';
  p = put_f2(p, b); *p++ = ',';
  p = put_f2(p, c); *p++ = ',';
  p = put_f2(p, d);
  *p++ = '!';
  send(buf, (uint16_t)(p - buf));
}

void Chassis_SetPwm(float a, float b, float c, float d)
{
  char buf[64];
  char *p = buf;

  /* 例程把 -1..1 乘 100 再发，驱动板收到的是 -100..100 */
  p = put_str(p, "$Car_Pwm:");
  p = put_f2(p, a * 100.0f); *p++ = ',';
  p = put_f2(p, b * 100.0f); *p++ = ',';
  p = put_f2(p, c * 100.0f); *p++ = ',';
  p = put_f2(p, d * 100.0f);
  *p++ = '!';
  send(buf, (uint16_t)(p - buf));
}

void Chassis_Stop(void)
{
  Chassis_SetSpeed(0.0f, 0.0f, 0.0f, 0.0f);
}

void Chassis_RequestBattery(void)
{
  const char *cmd = "$MOTOR_4CH_READ:battery!";
  send(cmd, (uint16_t)strlen(cmd));
}

uint32_t Chassis_BatteryMv(void) { return s_batt_mv; }
bool     Chassis_Online(void)    { return s_online; }

void Chassis_Tick1s(void)
{
  Chassis_RequestBattery();
}
