/**
  ******************************************************************************
  * @file    scurve.c
  * @brief   7 段 S 曲线规划与求值
  *
  * 加速段三段（jerk +j、0、-j）+ 匀速段 + 减速段三段（-j、0、+j）：
  *
  *    a(t) :  0 -> a_peak -> a_peak -> 0 -> 0 -> -a_peak -> -a_peak -> 0
  *    v(t) :  0 -> ................. v_peak ................ -> 0
  *
  * v_peak 不够高时（短行程）峰值加速度自动降下来，a_peak = sqrt(v_peak*j)，
  * 这时加速段只有 jerk 段、没有恒加速段 —— 行程更短时就是"纯 S"。
  ******************************************************************************
  */
#include "scurve.h"
#include <math.h>

/* 给定峰值速度，算出加速段三段的时长与位移 */
static float ramp_params(float v, float j, float a_cap,
                         float *t_j, float *t_a, float *a_peak)
{
  float ap, tj, ta, v1, v2, x1, x2, x3;

  if (v <= 0.0f)
  {
    *t_j = 0.0f; *t_a = 0.0f; *a_peak = 0.0f;
    return 0.0f;
  }

  ap = sqrtf(v * j);          /* 纯 S 时能达到的峰值加速度 */
  if (ap > a_cap) ap = a_cap; /* 有恒加速段时被 a_max 截住 */
  tj = ap / j;
  ta = v / ap - tj;
  if (ta < 0.0f) ta = 0.0f;

  v1 = 0.5f * ap * tj;
  x1 = j * tj * tj * tj / 6.0f;

  x2 = v1 * ta + 0.5f * ap * ta * ta;
  v2 = v1 + ap * ta;

  x3 = v2 * tj + 0.5f * ap * tj * tj - j * tj * tj * tj / 6.0f;

  *t_j = tj; *t_a = ta; *a_peak = ap;
  return x1 + x2 + x3;
}

bool scurve_plan(scurve_t *p, float dist, const scurve_lim_t *lim)
{
  float D, v, lo, hi, tj, ta, ap, xr, xr_max;
  int   i;

  if (p == 0 || lim == 0) return false;

  p->lim_v = lim->v_max;
  p->lim_a = lim->a_max;
  p->lim_j = lim->j_max;
  p->j     = lim->j_max;
  p->sgn   = (dist < 0.0f) ? -1.0f : 1.0f;
  D        = fabsf(dist);
  p->dist  = D;

  if (lim->v_max <= 0.0f || lim->a_max <= 0.0f || lim->j_max <= 0.0f || D <= 0.0f)
  {
    p->v_peak = p->a_peak = p->t_j = p->t_a = p->t_c = p->T = 0.0f;
    return false;
  }

  xr_max = ramp_params(lim->v_max, lim->j_max, lim->a_max, &tj, &ta, &ap);

  if (2.0f * xr_max <= D)
  {
    /* 够长，有匀速段 */
    p->v_peak = lim->v_max;
    p->t_c    = (D - 2.0f * xr_max) / lim->v_max;
    (void)ramp_params(p->v_peak, p->j, lim->a_max, &p->t_j, &p->t_a, &p->a_peak);
  }
  else
  {
    /* 短行程：二分找能达到的峰值速度（2*ramp(v) = D 单调递增） */
    lo = 0.0f;
    hi = lim->v_max;
    for (i = 0; i < 40; i++)
    {
      v  = 0.5f * (lo + hi);
      xr = ramp_params(v, p->j, lim->a_max, &tj, &ta, &ap);
      if (2.0f * xr < D) lo = v; else hi = v;
    }
    p->v_peak = 0.5f * (lo + hi);
    p->t_c    = 0.0f;
    (void)ramp_params(p->v_peak, p->j, lim->a_max, &p->t_j, &p->t_a, &p->a_peak);
  }

  p->T = 4.0f * p->t_j + 2.0f * p->t_a + p->t_c;
  return true;
}

void scurve_eval(const scurve_t *p, float t, float *pos, float *vel, float *acc)
{
  const float jk[7]  = { 1.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, 1.0f };
  float dur[7];
  float x = 0.0f, v = 0.0f, a = 0.0f, rem;
  int   k;

  dur[0] = p->t_j; dur[1] = p->t_a; dur[2] = p->t_j; dur[3] = p->t_c;
  dur[4] = p->t_j; dur[5] = p->t_a; dur[6] = p->t_j;

  if (t < 0.0f)  t = 0.0f;
  if (t > p->T)  t = p->T;
  rem = t;

  for (k = 0; k < 7; k++)
  {
    float seg = dur[k];
    float j   = jk[k] * p->j;
    float u   = (rem < seg) ? rem : seg;

    x += v * u + 0.5f * a * u * u + j * u * u * u / 6.0f;
    v += a * u + 0.5f * j * u * u;
    a += j * u;

    rem -= u;
    if (rem <= 0.0f) break;
  }

  if (pos) *pos = p->sgn * x;
  if (vel) *vel = p->sgn * v;
  if (acc) *acc = p->sgn * a;
}
