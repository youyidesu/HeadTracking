/**
  ******************************************************************************
  * @file    buzzer.c
  * @brief   无源蜂鸣器 PWM 发声 + 非阻塞旋律播放器
  ******************************************************************************
  */
#include "buzzer.h"

/* TIM3 句柄在 main.c 里定义（CubeMX 生成），这里只引用 */
extern TIM_HandleTypeDef htim3;

/* 计数分辨率：1 MHz -> 1 us 一格，ARR = 1000000 / f - 1
   16 位 ARR 上限 65535，因此最低可发 1000000/65536 ≈ 15.3 Hz
   （曲子最低音 B0 = 31 Hz，够用） */
#define BUZZER_TICK_HZ     1000000UL

/* 发声音量与"断音"参数 */
#define ARTIC_GAP_DIV      8u     /* 音与音之间的空隙 = 时值 / 8 */
#define ARTIC_GAP_MIN_MS   2u
#define ARTIC_GAP_MAX_MS   12u

/* MIDI 导出时"时值"往往等于到下一个音的时间隔，长值里混着休止。
   超过这个长度的音只发这么久的声，剩下的时间当静音，免得蜂鸣器一直嗡。 */
#define MAX_TONE_MS        1200u

static uint8_t s_pwm_started = 0;

/* -------------------------------------------------------------------------- */
/* 工具                                                                        */
/* -------------------------------------------------------------------------- */

/* TIM3 的计数时钟：APB1 预分频 != 1 时，定时器时钟 = 2 x PCLK1 */
static uint32_t tim3_input_hz(void)
{
  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

  if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_HCLK_DIV1)
  {
    pclk1 *= 2u;
  }
  return pclk1;
}

/* -------------------------------------------------------------------------- */
/* 发声                                                                        */
/* -------------------------------------------------------------------------- */

void Buzzer_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  /* PA6 -> AF2(TIM3_CH1)。
     如果 CubeMX 里已经把 PA6 配成 S_TIM3_CH1，这段是幂等的；
     如果它还留在 GPIO_Output（本工程 .ioc 的原始状态），这里会把 PA6
     从普通输出改成复用推挽，否则 PWM 根本出不到引脚上。 */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin       = BUZZER_PIN;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = BUZZER_PIN_AF;
  HAL_GPIO_Init(BUZZER_PORT, &gpio);

  /* PSC 由实际时钟算出来，改时钟树不用改这里 */
  __HAL_TIM_SET_PRESCALER(&htim3, (uint32_t)(tim3_input_hz() / BUZZER_TICK_HZ) - 1u);
  __HAL_TIM_SET_AUTORELOAD(&htim3, 0xFFFFu);
  __HAL_TIM_SET_COMPARE(&htim3, BUZZER_TIM_CHANNEL, 0u);   /* 占空比 0 = 无声 */
  htim3.Instance->EGR = TIM_EGR_UG;   /* PSC/ARR 有影子寄存器，先产生一次更新事件 */

  if (HAL_TIM_PWM_Start(&htim3, BUZZER_TIM_CHANNEL) != HAL_OK)
  {
    Error_Handler();
  }
  s_pwm_started = 1;
}

void Buzzer_Tone(uint16_t freq_hz)
{
  uint32_t arr;

  if (freq_hz < 20u)          /* 0 = 休止；20Hz 以下人耳也听不到 */
  {
    Buzzer_Silence();
    return;
  }

  arr = (BUZZER_TICK_HZ + (freq_hz / 2u)) / freq_hz;   /* 四舍五入 */
  if (arr < 20u)
  {
    arr = 20u;                /* 频率过高时截断，保证占空比还有几级 */
  }
  if (arr > 65536u)
  {
    arr = 65536u;
  }

  __HAL_TIM_SET_AUTORELOAD(&htim3, arr - 1u);
  __HAL_TIM_SET_COMPARE(&htim3, BUZZER_TIM_CHANNEL, arr / 2u);   /* 50% 占空比最响 */
  htim3.Instance->EGR = TIM_EGR_UG;   /* ARR 有预装载，产生更新事件让新值立刻生效 */

  if (!s_pwm_started)
  {
    HAL_TIM_PWM_Start(&htim3, BUZZER_TIM_CHANNEL);
    s_pwm_started = 1;
  }
}

void Buzzer_Silence(void)
{
  /* 只把占空比打到 0，不停定时器：省掉每次启动/停止 PWM 的开销，也不会咔哒 */
  if (s_pwm_started)
  {
    __HAL_TIM_SET_COMPARE(&htim3, BUZZER_TIM_CHANNEL, 0u);
  }
}

void Buzzer_Beep(uint16_t freq_hz, uint32_t ms)
{
  Buzzer_Tone(freq_hz);
  HAL_Delay(ms);
  Buzzer_Silence();
}

/* -------------------------------------------------------------------------- */
/* 非阻塞播放器                                                                */
/* -------------------------------------------------------------------------- */

static const Melody_t *s_mel   = 0;
static uint16_t        s_idx   = 0;
static uint32_t        s_t0    = 0;   /* 本阶段开始时刻 */
static uint16_t        s_on_ms = 0;   /* 发声段长度 */
static uint16_t        s_off_ms= 0;   /* 静音段长度 = 音间空隙 + 超长音的剩余部分 */
static uint8_t         s_phase = 0;   /* 0 = 正在发声, 1 = 静音段 */
static uint8_t         s_active= 0;

/* 装填第 idx 个音 */
static void load_note(uint16_t idx)
{
  uint16_t d;
  uint16_t gap;

  if ((s_mel == 0) || (idx >= s_mel->count))
  {
    return;
  }

  d = s_mel->dur_ms[idx];

  /* 音与音之间留一点空隙，同音高的连续音才分得开 */
  gap = (uint16_t)(d / ARTIC_GAP_DIV);
  if (gap < ARTIC_GAP_MIN_MS) gap = ARTIC_GAP_MIN_MS;
  if (gap > ARTIC_GAP_MAX_MS) gap = ARTIC_GAP_MAX_MS;
  if (d <= ARTIC_GAP_MIN_MS)  gap = 0;             /* 太短的音不留空 */

  s_on_ms = (uint16_t)(d - gap);
  if (s_on_ms > MAX_TONE_MS)
  {
    s_on_ms = MAX_TONE_MS;   /* 超长时值只发 1.2s 声，剩下转成静音 */
  }

  /* 关键：静音段 = 整个时值 - 发声段，这样总时间轴才和 MIDI 一致
     （19200 ms 这种"音符+长休止"的项不会把后面的音挤到一起） */
  s_off_ms = (uint16_t)(d - s_on_ms);

  Buzzer_Tone(s_mel->freq_hz[idx]);   /* 休止符(0)会自动静音，但仍占用时长 */
  s_t0    = HAL_GetTick();
  s_phase = 0;
}

void Melody_Play(const Melody_t *m)
{
  if ((m == 0) || (m->count == 0u))
  {
    return;
  }
  s_mel    = m;
  s_idx    = 0;
  s_active = 1;
  load_note(0);
}

void Melody_Stop(void)
{
  s_active = 0;
  s_mel    = 0;
  Buzzer_Silence();
}

void Melody_Poll(void)
{
  uint32_t elapsed;

  if (!s_active || (s_mel == 0))
  {
    return;
  }

  elapsed = HAL_GetTick() - s_t0;   /* 无符号相减，跨 tick 回绕也安全 */

  if (s_phase == 0)
  {
    if (elapsed < s_on_ms)
    {
      return;
    }
    Buzzer_Silence();               /* 到点先断音，制造音与音之间的颗粒感 */
    s_t0    = HAL_GetTick();
    s_phase = 1;
    if (s_off_ms != 0u)
    {
      return;                       /* 还有静音段，下一轮再判 */
    }
  }
  else
  {
    if (elapsed < s_off_ms)
    {
      return;
    }
  }

  /* 进入下一个音 */
  s_idx++;
  if (s_idx >= s_mel->count)
  {
    if (s_mel->loop)
    {
      s_idx = 0;
    }
    else
    {
      Melody_Stop();
      return;
    }
  }
  load_note(s_idx);
}

uint8_t Melody_IsPlaying(void)
{
  return s_active;
}

uint16_t Melody_Progress(void)
{
  return s_idx;
}
