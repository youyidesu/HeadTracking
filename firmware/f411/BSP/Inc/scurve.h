/**
  ******************************************************************************
  * @file    scurve.h
  * @brief   7 段 S 曲线（jerk 限幅）速度剖面：静止 -> 静止，双轴通用
  *
  * 单位统一用 度 / 秒：
  *     v_max  deg/s      a_max  deg/s^2      j_max  deg/s^3
  *     rpm -> deg/s :  deg = rpm * 6
  ******************************************************************************
  */
#ifndef __SCURVE_H
#define __SCURVE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
  float v_max;   /* deg/s   */
  float a_max;   /* deg/s^2 */
  float j_max;   /* deg/s^3 */
} scurve_lim_t;

typedef struct
{
  float lim_v, lim_a, lim_j;
  float sgn;      /* 位移方向 */
  float dist;     /* |总位移|，度 */
  float v_peak;   /* 峰值速度 deg/s */
  float a_peak;   /* 峰值加速度 deg/s^2 */
  float j;        /* 实际用的 jerk（= j_max） */
  float t_j;      /* jerk 段 */
  float t_a;      /* 恒加速度段 */
  float t_c;      /* 匀速段 */
  float T;        /* 总时长 */
} scurve_t;

/* 规划一段 rest->rest 运动。dist 带符号；返回 false 表示参数非法（T 置 0） */
bool scurve_plan(scurve_t *p, float dist, const scurve_lim_t *lim);

/* 在 t 时刻求值（t 会夹到 [0,T]） */
void scurve_eval(const scurve_t *p, float t, float *pos, float *vel, float *acc);

/* 便捷：跑了多久给个百分比 0..1 */
static inline float scurve_progress(const scurve_t *p, float t)
{
  return (p->T > 0.0f) ? (t / p->T) : 1.0f;
}

#endif /* __SCURVE_H */
