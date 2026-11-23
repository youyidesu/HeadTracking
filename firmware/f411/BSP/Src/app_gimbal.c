/**
  ******************************************************************************
  * @file    app_gimbal.c
  * @brief   两轴云台：ZDT 闭环步进 + S 曲线 + 小幅度演示动作
  *
  * 运动方式：位置模式流式下发（0xFD, 绝对位置）。
  *   200 Hz 把 S 曲线上的瞬时位置换算成脉冲数发出去，驱动器自己跟；
  *   加速度参数给 0（拉满），让"平滑"完全由我们这边的 S 曲线决定，
  *   不会被驱动器内部的梯形规划再滤一遍。
  ******************************************************************************
  */
#include "app_gimbal.h"
#include "esp_link.h"
#include <math.h>

/* ---------------- 机械/电气常量 ---------------- */
#define MOTOR_STEP_DEG      1.8f          /* 换成 0.9° 电机时改成 0.9f */
#define MICROSTEP           16            /* ZDT 拨码细分 */
#define PULSES_PER_REV      (360.0f / MOTOR_STEP_DEG * (float)MICROSTEP)
#define PULSES_PER_DEG      (PULSES_PER_REV / 360.0f)     /* 1.8°/16 -> 8.889 */

/* 两轴各占一条 UART（滑环 6 线刚好够，不用并总线）：
     YAW   -> USART6 (PA11/PA12) 车体侧，过滑环
     PITCH -> USART1 (PA9/PA10)  炮塔侧，本地短线
   两条都是出厂地址 1，不用改驱动器。 */
#define YAW_ADDR            1
#define PITCH_ADDR          1

/* ---------------- 速度上限（度、秒） ----------------
 * YAW：42ZDT28A（官方 0.22 N·m / 180 g / 0.8 A / 10 Ω / 4.6 mH，闭环）
 *      按官方 R/L/电流算，12 V 下 150 rpm 可给 0.187 N·m，需求 0.132 → 余量 1.4×。
 *      200 rpm 可给 0.152，需求 0.162：左侧支架收到 ≤40 mm（J≤1.25e-3）时成立，
 *      仍挂在 70 mm 就只能到 150 rpm。
 *      jerk 取 25000：长细 yaw 轴一阶扭转模态约 33~40 Hz，40000 会激励它。
 *      换 Ø8 轴或加双支撑后可放回 40000；出现回弹余振则降到 15000。
 * PITCH：42ZDT40A（官方 0.43 N·m / 285 g / 2 A / 2 Ω / 3.6 mH）
 *      12 V 下 120 rpm 可给 0.285，12 V/200 rpm 仍有 0.170——需求只要 ~0.04，
 *      所以 PITCH 是被严重超配的，跑 120 rpm 只是为跟头的响应感。
 */
static const scurve_lim_t k_lim_yaw   = {  900.0f, 4000.0f, 25000.0f }; /* 150 rpm */
static const scurve_lim_t k_lim_pitch = {  720.0f, 2000.0f, 20000.0f }; /* 120 rpm */

/* 流式位置指令里带的速度/加速度参数（驱动器侧限幅） */
#define STREAM_RPM          300
#define STREAM_ACC          0

/* ---------------- 头瞄跟随 / 链路保护 ---------------- */
#define EXTRAP_MAX_MS       30u     /* 最多按角速度外推 30ms（约 3 帧） */
#define LINK_LOST_MS        150u    /* 超时 → 冻结，保持最后位置 */
#define LINK_DEAD_MS        1000u   /* 超时 → 停车并进 FAULT */
#define REACQ_ERR_DEG       45.0f   /* 误差大于此值就降速追，避免重连后突然甩 */
#define REACQ_VMAX_DPS      300.0f
#define TRACK_TAU_S         0.05f   /* 跟随时间常数：响应快但不振 */

/* ---------------- 上电限位自校准（PITCH） ----------------
 * 思路：开机让 pitch 慢速往外扫，遇到机械挡块（驱动器报堵转/限位，或
 * 编码器连续几个采样点不动）就认定那里是机械极值；退回来一点，
 * 把退回来的位置设为本次开机的原点（驱动器 0x0A 清零）。
 * 以后加陀螺仪时，这个机械原点还能当"绝对参考"用来校正零漂。
 */
#define HOME_STEP_DEG      2.0f    /* 每次试探的角度步长 */
#define HOME_MAX_DEG       30.0f   /* 单方向最多扫这么多，防撞坏 */
#define HOME_BACKOFF_DEG   6.0f    /* 碰到挡块后退回多少 */
#define HOME_STALL_N       25      /* 连续 25 个采样（500ms）没动才判堵转 */
#define HOME_MOVE_RPM      15      /* 试探速度：慢，撞到也不疼 */
#define HOME_CENTER_DEG    18.0f   /* 校准完从限位退回多少度当正角度 */

/* 轴方向：+1 = 与头姿同向，-1 = 反向（电机/齿轮装配方向相反时改这里） */
#define YAW_SIGN            (+1.0f)
#define PITCH_SIGN          (-1.0f)    /* 实测俯仰反向 */

/* 演示结束后的停靠位：给机械装配一个确定的参考角
   想预紧消回差可以填 +2~+5°，想正前方就留 0 */
#define HOME_YAW_DEG        0.0f
#define HOME_PITCH_DEG      0.0f

typedef enum { MODE_DEMO = 0, MODE_TRACK, MODE_FAULT } gimbal_mode_t;
static gimbal_mode_t s_mode = MODE_DEMO;

static qp_head_t s_head;
static volatile uint8_t s_head_ok;      /* 有没有收到过合法帧 */
static uint32_t  s_head_tick;           /* 最后一帧的时刻 */
static float     s_zero_yaw, s_zero_pitch;   /* Recenter 偏置 */
static uint8_t   s_estop_latched;            /* 急停锁存：必须按回中才解除 */
static uint8_t   s_park_latched;             /* 停靠锁存：停靠期间不被头瞄帧抢走 */

/* ---------------- 演示动作表 ---------------- */
typedef struct
{
  float yaw_deg;
  float pitch_deg;
  uint16_t hold_ms;
} step_t;

static const step_t k_demo[] =
{
  {  15.0f,  10.0f, 300 },
  { -15.0f, -10.0f, 300 },
  {   0.0f,   0.0f, 600 },
};
#define DEMO_STEPS   (sizeof(k_demo) / sizeof(k_demo[0]))
#define DEMO_LOOPS   3

/* ---------------- 单轴状态 ---------------- */
typedef struct
{
  zdt_uart_t uart;
  const scurve_lim_t *lim;
  scurve_t   sc;
  float      t;          /* 本段已经走过的时间 */
  float      from_deg;
  float      to_deg;
  uint8_t    moving;

  /* 头瞄跟随用 */
  float      cmd;        /* 当前指令角（度） */
  float      vel;        /* 当前指令角速度（度/秒） */
} axis_t;

static axis_t s_yaw, s_pitch;

/* 限位校准子状态：0=正向扫 1=反向扫 2=回退 3=完成 */
static uint8_t  s_home_st;
static float    s_home_tgt;
static float    s_home_last;
static float    s_home_stop;
static uint8_t  s_home_stall;
static uint8_t  s_home_tried_neg;
static uint8_t  s_home_ok;

static uint8_t  s_init_stage;   /* 0=等上电 1=使能 2=清零 7=限位校准 3=跑演示 4=结束 */
static int8_t   s_home_dir;     /* +1=向正方向扫到限位，-1=反向 */
/* yaw 连续角：跨正负 180 度不跳变 */
static float    s_yaw_cont;
static float    s_yaw_prev;
static uint8_t  s_yaw_cont_init;
static uint8_t  s_home_hits;    /* 本次校准撞到限位的次数，0=一次都没撞到 */
static uint32_t s_stage_t0;
static uint8_t  s_step;         /* 当前第几个动作 */
static uint8_t  s_loop;         /* 第几轮 */
static uint32_t s_hold_t0;
static uint8_t  s_holding;

/* -------------------------------------------------------------------------- */
/* 内部                                                                       */
/* -------------------------------------------------------------------------- */

static void axis_start(axis_t *a, float to_deg)
{
  a->from_deg = a->to_deg;
  a->to_deg   = to_deg;
  a->t        = 0.0f;
  a->moving   = 1u;
  (void)scurve_plan(&a->sc, to_deg - a->from_deg, a->lim);
}

/* pitch 当前位置（度），来自闭环编码器 */
static float pitch_pos_deg(void)
{
  return (float)s_pitch.uart.pos_raw / PULSES_PER_DEG;
}

/* 给 pitch 下一个绝对角度指令（相对驱动器零点） */
static void pitch_move_deg(float deg)
{
  int32_t pulses = (int32_t)lroundf(deg * (double)PULSES_PER_DEG);
  zdt_uart_move(&s_pitch.uart, pulses, HOME_MOVE_RPM, 0, ZDT_MODE_ABS);
}

static void home_begin(void)
{
  s_home_st        = 0u;
  s_home_tgt       = 0.0f;
  s_home_hits      = 0u;
  s_home_last      = 0.0f;
  s_home_stop      = 0.0f;
  s_home_stall     = 0u;
  s_home_tried_neg = 0u;
  s_home_ok        = 0u;
  pitch_move_deg(0.0f);
}

/* 20ms 调一次。返回 1 表示校准结束（成功或放弃） */
static uint8_t home_poll(void)
{
  float pos  = pitch_pos_deg();
  uint8_t limited = (s_pitch.uart.last_status == ZDT_ST_LIMIT);

  /* 编码器不动也算堵转（有些固件不回报 0x22）。
     但必须先真的下发过位移，否则刚上电静止就被误判成"已到限位"。 */
  if (fabsf(pos - s_home_last) < 0.15f && fabsf(s_home_tgt) >= HOME_STEP_DEG) s_home_stall++;
  else                                                              s_home_stall = 0u;
  s_home_last = pos;

  uint8_t blocked = limited || (s_home_stall >= HOME_STALL_N);

  switch (s_home_st)
  {
    case 0:                                   /* 正向扫 */
    case 1:                                   /* 反向扫 */
      if (blocked)
      {
        s_home_hits++;
        s_home_dir  = (s_home_st == 0u) ? 1 : -1;
        s_home_stop = s_home_tgt;             /* 极值就在这个指令位置附近 */
        s_home_st   = 2u;                     /* 去回退 */
        s_home_stall = 0u;
        return 0u;
      }
      s_home_tgt += (s_home_st == 0u) ? HOME_STEP_DEG : -HOME_STEP_DEG;
      if (fabsf(s_home_tgt) > HOME_MAX_DEG)
      {
        if (s_home_st == 0u && !s_home_tried_neg)
        {
          s_home_tried_neg = 1u;
          s_home_st  = 1u;                    /* 正向没碰到，换反向再试 */
          s_home_tgt = 0.0f;
        }
        else
        {
          s_home_st = 3u;                     /* 两边都扫完还没挡住：放弃 */
          return 1u;
        }
      }
      pitch_move_deg(s_home_tgt);
      return 0u;

    case 2: {                                 /* 回退并定原点 */
      float back = (s_home_stop >= 0.0f) ? (s_home_stop - HOME_BACKOFF_DEG)
                                         : (s_home_stop + HOME_BACKOFF_DEG);
      pitch_move_deg(back);

      /* 等它到位，再把这点设成 0（驱动器 0x0A） */
      static uint32_t t0;
      static uint8_t  sent;
      if (!sent) { t0 = HAL_GetTick(); sent = 1u; }
      if ((uint32_t)(HAL_GetTick() - t0) >= 400u)
      {
        uint8_t f[8];
        uint16_t n = zdt_frame_zero(PITCH_ADDR, f);
        zdt_uart_send(&s_pitch.uart, f, n);
        s_home_ok = 1u;
        s_home_st = 4u;                       /* 继续去正角度位置 */
        sent = 0u;
        return 0u;
      }
      return 0u;
    }

    case 4: {                               /* 从限位退到正角度 */
      float target = (s_home_dir >= 0) ? -HOME_CENTER_DEG : HOME_CENTER_DEG;
      pitch_move_deg(target);

      static uint32_t t4;
      static uint8_t  s4;
      if (!s4) { t4 = HAL_GetTick(); s4 = 1u; }
      if ((uint32_t)(HAL_GetTick() - t4) >= 900u)
      {
        s4 = 0u;
        s_home_st = 3u;
        return 1u;
      }
      return 0u;
    }

    default:
      return 1u;
  }
}

/* 把当前瞬时位置换算成脉冲发出去 */
static void axis_stream(axis_t *a)
{
  float pos, vel, acc;
  int32_t pulses;
  int16_t rpm;

  scurve_eval(&a->sc, a->t, &pos, &vel, &acc);
  pulses = (int32_t)lroundf((double)(a->from_deg + pos) * (double)PULSES_PER_DEG);

  /* 速度限幅取当前剖面速度（1 rpm = 6 deg/s），这样驱动器每个 5ms 只走
     剖面允许的那点距离，不会"扑到目标再等"，200Hz 台阶就被抹平了 */
  rpm = (int16_t)lroundf(fabsf(vel) / 6.0f);
  if (rpm < 5) rpm = 5;
  if (rpm > STREAM_RPM) rpm = STREAM_RPM;

  zdt_uart_move(&a->uart, pulses, (uint16_t)rpm, STREAM_ACC, ZDT_MODE_ABS);
}

static void start_step(uint8_t idx)
{
  s_step = idx;
  axis_start(&s_yaw,   k_demo[idx].yaw_deg);
  axis_start(&s_pitch, k_demo[idx].pitch_deg);
  s_holding = 0;
}


/* -------------------------------------------------------------------------- */
/* 头瞄跟随                                                                    */
/* -------------------------------------------------------------------------- */

/* 限速+限加速的跟随器：不会因为目标突变而甩过去 */
static void track_step(axis_t *a, float target, float dt, float vmax, float amax)
{
  float err = target - a->cmd;
  float vm  = (fabsf(err) > REACQ_ERR_DEG) ? REACQ_VMAX_DPS : vmax;
  float v_des = err / TRACK_TAU_S;
  float dv;

  if (v_des >  vm) v_des =  vm;
  if (v_des < -vm) v_des = -vm;

  dv = v_des - a->vel;
  if (dv >  amax * dt) dv =  amax * dt;
  if (dv < -amax * dt) dv = -amax * dt;
  a->vel += dv;

  a->cmd += a->vel * dt;

  /* 收敛：误差很小就贴住目标，避免长期微抖 */
  if (fabsf(target - a->cmd) < 0.15f && fabsf(a->vel) < 4.0f)
  {
    a->cmd = target;
    a->vel = 0.0f;
  }
}

/* 把当前指令角发下去 */
static void track_send(axis_t *a)
{
  int32_t pulses = (int32_t)lroundf((double)a->cmd * (double)PULSES_PER_DEG);
  int16_t rpm    = (int16_t)lroundf(fabsf(a->vel) / 6.0f);

  if (rpm < 5)   rpm = 5;
  if (rpm > 400) rpm = 400;

  zdt_uart_move(&a->uart, pulses, (uint16_t)rpm, STREAM_ACC, ZDT_MODE_ABS);
}

static void gimbal_stop_all(void)
{
  zdt_uart_stop(&s_yaw.uart);
  zdt_uart_stop(&s_pitch.uart);
  s_yaw.vel = s_pitch.vel = 0.0f;
}

/* 进入跟随：把指令角对齐到当前目标，避免切换瞬间甩动 */
static void gimbal_enter_track(void)
{
  s_yaw_cont_init = 0u;      /* 重新以当前姿态为基准 */
  float age = (float)(HAL_GetTick() - s_head_tick) * 0.001f;
  float y, p;

  if (age > (float)EXTRAP_MAX_MS * 0.001f) age = (float)EXTRAP_MAX_MS * 0.001f;

  y = s_head.yaw_cdeg   * 0.01f + (s_head.yaw_rate   * 0.1f) * age - s_zero_yaw;
  p = s_head.pitch_cdeg * 0.01f + (s_head.pitch_rate * 0.1f) * age - s_zero_pitch;

  s_yaw.cmd   = y;  s_yaw.vel   = 0.0f;
  s_pitch.cmd = p;  s_pitch.vel = 0.0f;
  s_mode = MODE_TRACK;
}

/* 跟随主任务（100 Hz） */
static float wrap180(float a)
{
  while (a > 180.0f)  a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

static void track_task(uint32_t now)
{
  uint32_t age = now - s_head_tick;
  float    dt  = 0.01f;          /* 与 Tick5ms/2 的调用节奏一致 */
  float    ex, y, p;

  /* 链路死了：停车进 FAULT，等恢复 */
  if (age > LINK_DEAD_MS)
  {
    gimbal_stop_all();
    s_mode = MODE_FAULT;
    return;
  }

  if (age > LINK_LOST_MS)
  {
    return;                      /* 冻结：不发新指令，ZDT 保持当前位置 */
  }

  /* 外推（最多 30ms），再减零点偏置 */
  ex = (float)age * 0.001f;
  if (ex > (float)EXTRAP_MAX_MS * 0.001f) ex = (float)EXTRAP_MAX_MS * 0.001f;

  /* 先用绝对 yaw 累加成连续角，避免跨 180 度时电机转整圈 */
  {
    float raw = s_head.yaw_cdeg * 0.01f - s_zero_yaw;
    if (!s_yaw_cont_init) { s_yaw_cont = raw; s_yaw_prev = raw; s_yaw_cont_init = 1u; }
    s_yaw_cont += wrap180(raw - s_yaw_prev);
    s_yaw_prev  = raw;
  }

  y = (s_yaw_cont + (s_head.yaw_rate * 0.1f) * ex) * YAW_SIGN;
  p = (s_head.pitch_cdeg * 0.01f + (s_head.pitch_rate * 0.1f) * ex - s_zero_pitch) * PITCH_SIGN;

  /* 软限位：pitch ±45°，yaw 不限 */
  if (p >  45.0f) p =  45.0f;
  if (p < -45.0f) p = -45.0f;

  track_step(&s_yaw,   y, dt, k_lim_yaw.v_max,   k_lim_yaw.a_max);
  track_step(&s_pitch, p, dt, k_lim_pitch.v_max, k_lim_pitch.a_max);

  track_send(&s_yaw);
  track_send(&s_pitch);
}

/* -------------------------------------------------------------------------- */
/* 对外                                                                       */
/* -------------------------------------------------------------------------- */

uint8_t Gimbal_Mode(void)
{
  if (s_init_stage == 7u) return 0x07u;          /* 正在做限位校准 */
  if (s_init_stage < 2u)  return 0x06u;          /* 上电使能/清零中 */
  if (s_home_hits == 0u)  return 0x08u;          /* 校准跑完但没撞到任何限位 */
  return (uint8_t)s_mode;
}

/* 给 C5 回读用的驱动遥测：编码器位置 + 应答统计 */
void Gimbal_Diag(float *yaw_deg, float *pitch_deg,
                 uint16_t *yaw_ack, uint16_t *pitch_ack,
                 uint16_t *yaw_err, uint16_t *pitch_err,
                 uint8_t *yaw_st, uint8_t *pitch_st,
                 uint16_t *yaw_tx, uint16_t *pitch_tx)
{
  *yaw_tx   = (uint16_t)(s_yaw.uart.n_tx & 0xFFFF);
  *pitch_tx = (uint16_t)(s_pitch.uart.n_tx & 0xFFFF);
  *yaw_deg   = (float)s_yaw.uart.pos_raw   / PULSES_PER_DEG;
  *pitch_deg = (float)s_pitch.uart.pos_raw / PULSES_PER_DEG;
  *yaw_ack   = (uint16_t)(s_yaw.uart.n_ack & 0xFFFF);
  *pitch_ack = (uint16_t)(s_pitch.uart.n_ack & 0xFFFF);
  *yaw_err   = (uint16_t)(s_yaw.uart.n_err & 0xFFFF);
  *pitch_err = (uint16_t)(s_pitch.uart.n_err & 0xFFFF);
  *yaw_st    = s_yaw.uart.last_status;
  *pitch_st  = s_pitch.uart.last_status;
}

void Gimbal_Init(void)
{
  zdt_uart_init(&s_yaw.uart,   &huart6, YAW_ADDR);    /* 过滑环，115200 */
  zdt_uart_init(&s_pitch.uart, &huart1, PITCH_ADDR);  /* 炮塔本地，115200 */

  s_yaw.lim   = &k_lim_yaw;
  s_pitch.lim = &k_lim_pitch;
  s_yaw.to_deg = s_pitch.to_deg = 0.0f;
  s_yaw.moving = s_pitch.moving = 0u;

  s_init_stage = 0u;
  s_stage_t0   = HAL_GetTick();
  s_loop = 0u;

  s_mode      = MODE_DEMO;
  s_estop_latched = 0u;
  s_park_latched  = 0u;
  s_head_ok   = 0u;
  s_zero_yaw  = 0.0f;
  s_zero_pitch= 0.0f;
  s_yaw.cmd   = s_yaw.vel   = 0.0f;
  s_pitch.cmd = s_pitch.vel = 0.0f;
}

void Gimbal_Tick1ms(void)
{
  zdt_uart_poll(&s_yaw.uart);
  zdt_uart_poll(&s_pitch.uart);

  Esp_Watchdog();                       /* 断流 500ms 自动重挂 SPI */

  if (Esp_GetHead(&s_head))             /* 收到新帧 */
  {
    s_head_tick = HAL_GetTick();
    s_head_ok   = 1u;

    /* ESTOP：立即停并锁存，松开急停 + 按回中才允许恢复 */
    if (s_head.flags & QP_F_ESTOP)
    {
      gimbal_stop_all();
      s_mode = MODE_FAULT;
      s_estop_latched = 1u;
    }
    else if (s_estop_latched && (s_head.flags & QP_F_RECENTER))
    {
      s_estop_latched = 0u;              /* 人工复位：清锁存，下一帧自动重进跟随 */
      s_zero_yaw   += s_head.yaw_cdeg   * 0.01f;
      s_zero_pitch += s_head.pitch_cdeg * 0.01f;
    }
    /* 普通 RECENTER：把当前头姿设为零点 */
    else if (s_head.flags & QP_F_RECENTER)
    {
      s_zero_yaw   += s_head.yaw_cdeg   * 0.01f;
      s_zero_pitch += s_head.pitch_cdeg * 0.01f;
    }
  }
}

void Gimbal_Tick5ms(void)
{
  uint32_t now = HAL_GetTick();
  static uint8_t s_div = 0u;
  static uint8_t s_pos_div = 0u;

  /* 20 Hz 读一次闭环位置（40 个 5ms tick），给 C5 回读用 */
  if (++s_pos_div >= 40u)
  {
    s_pos_div = 0u;
    zdt_uart_request_pos(&s_yaw.uart);
    zdt_uart_request_pos(&s_pitch.uart);
  }

  /* 跟随 / 故障恢复：100 Hz 更新 */
  if (s_mode == MODE_TRACK)
  {
    if (++s_div >= 2u) { s_div = 0u; track_task(now); }
    return;
  }

  if (s_mode == MODE_FAULT)
  {
    /* 链路恢复（连续收到帧）就自动重新进入跟随；急停锁存期间不恢复 */
    if (!s_estop_latched && !s_park_latched && s_head_ok &&
        (uint32_t)(now - s_head_tick) < LINK_LOST_MS)
    {
      gimbal_enter_track();
    }
    return;
  }

  /* 演示阶段：一旦有合法头瞄帧，立刻切跟随（停靠锁存期间除外） */
  if (!s_park_latched && s_head_ok && (uint32_t)(now - s_head_tick) < LINK_LOST_MS)
  {
    gimbal_enter_track();
    return;
  }

  /* ---- 上电时序：等 300ms -> 使能 -> 等 200ms -> 两个轴清零 -> 演示 ---- */
  switch (s_init_stage)
  {
    case 0:
      if ((now - s_stage_t0) >= 300u)
      {
        zdt_uart_enable(&s_yaw.uart, true);
        zdt_uart_enable(&s_pitch.uart, true);
        s_init_stage = 1u;
        s_stage_t0   = now;
      }
      return;

    case 1:
      if ((now - s_stage_t0) >= 200u)
      {
        uint8_t f[8];
        uint16_t n;

        n = zdt_frame_zero(YAW_ADDR, f);
        zdt_uart_send(&s_yaw.uart, f, n);

        n = zdt_frame_zero(PITCH_ADDR, f);
        zdt_uart_send(&s_pitch.uart, f, n);

        /* 清零完成 -> 做 pitch 机械限位自校准 */
        s_init_stage = 7u;
        s_stage_t0   = now;
        home_begin();
      }
      return;

    case 7:                                   /* 限位自校准：20ms 一次 */
      if ((uint32_t)(now - s_stage_t0) < 20u) return;
      s_stage_t0 = now;
      if (home_poll())
      {
        s_init_stage = 2u;                    /* 校准完成/放弃 -> 进演示 */
        s_stage_t0   = now;
      }
      return;

    case 2:
      if ((now - s_stage_t0) >= 200u)
      {
        s_init_stage = 3u;
        start_step(0u);
        s_hold_t0 = now;
      }
      return;

    case 3:
    case 5:
      break;                    /* 3=演示推进；5=停靠推进 */

    default:
      return;                   /* 4/6：停住，不再下发指令 */
  }

  /* ---- 演示：推进 S 曲线 ---- */
  /* 两轴共一条 115200 总线：13B 帧阻塞约 1.1ms，200Hz 会把环路吃满，
     所以位置流下发降到 100Hz；S 曲线本身仍按 5ms 步进求值。 */
  static uint8_t div = 0u;
  uint8_t send_now = 0u;

  if (++div >= 2u) { div = 0u; send_now = 1u; }

  if (s_yaw.moving)
  {
    s_yaw.t += 0.005f;
    if (s_yaw.t >= s_yaw.sc.T) { s_yaw.t = s_yaw.sc.T; s_yaw.moving = 0u; send_now = 1u; }
    if (send_now) axis_stream(&s_yaw);
  }

  if (s_pitch.moving)
  {
    s_pitch.t += 0.005f;
    if (s_pitch.t >= s_pitch.sc.T) { s_pitch.t = s_pitch.sc.T; s_pitch.moving = 0u; send_now = 1u; }
    if (send_now) axis_stream(&s_pitch);
  }

  /* ---- 停靠动作走完：进入 stage 6，彻底静默 ---- */
  if (s_init_stage == 5u && !s_yaw.moving && !s_pitch.moving)
  {
    s_init_stage = 6u;
    return;
  }

  /* ---- 两轴都停了就按 hold_ms 停留，然后进下一动作 ---- */
  if (!s_yaw.moving && !s_pitch.moving)
  {
    if (!s_holding)
    {
      s_holding  = 1u;
      s_hold_t0  = now;
    }
    else if ((now - s_hold_t0) >= k_demo[s_step].hold_ms)
    {
      uint8_t nxt = (uint8_t)(s_step + 1u);

      if (nxt < DEMO_STEPS)
      {
        start_step(nxt);
        s_hold_t0 = now;
      }
      else
      {
        s_loop++;
        if (s_loop < DEMO_LOOPS)
        {
          start_step(0u);
          s_hold_t0 = now;
        }
        else
        {
          /* 演示结束 → 走 S 曲线停到 HOME 角，停稳后不再发指令 */
          axis_start(&s_yaw,   HOME_YAW_DEG);
          axis_start(&s_pitch, HOME_PITCH_DEG);
          s_hold_t0    = now;
          s_init_stage = 5u;
        }
      }
    }
  }
}

uint8_t Gimbal_DemoDone(void)
{
  return (s_init_stage >= 5u) ? 1u : 0u;   /* 停靠中/停靠完成都算演示结束 */
}

/* 外部随时可以让它回停靠位（比如 C5 下发归中、或调试器里手动调） */
void Gimbal_Park(void)
{
  if (s_mode == MODE_FAULT) return;

  s_mode         = MODE_DEMO;     /* TRACK 分支会在 Tick5ms 开头早退，先切出来 */
  s_park_latched = 1u;            /* 停靠期间不让头瞄帧抢回去 */
  axis_start(&s_yaw,   HOME_YAW_DEG);
  axis_start(&s_pitch, HOME_PITCH_DEG);
  s_init_stage = 5u;
  s_hold_t0    = HAL_GetTick();
}

/* 解除停靠锁存，重新让头瞄接管 */
void Gimbal_Track(void)
{
  s_park_latched = 0u;
  s_init_stage   = 0u;            /* 走一遍使能/清零也不需要，直接进跟随由 head 帧触发 */
  gimbal_enter_track();
}
