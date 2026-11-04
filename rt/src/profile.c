#include "profile.h"

#include <math.h>

void profile_init(profile_t *p, double vmax, double accel, double stop_dec, double dt)
{
   p->pos = 0;
   p->frac = 0;
   p->vel = 0;
   p->vmax = vmax;
   p->accel = accel;
   p->stop_dec = stop_dec;
   p->dt = dt;
}

void profile_sync(profile_t *p, int32_t actual)
{
   p->pos = actual;
   p->frac = 0;
   p->vel = 0;
}

void profile_step(profile_t *p, double v_target, bool hard_stop)
{
   if (v_target > p->vmax) v_target = p->vmax;
   if (v_target < -p->vmax) v_target = -p->vmax;

   /* Đang giảm độ lớn vận tốc (hoặc đổi chiều) thì là giảm tốc. */
   bool decelerating = fabs(v_target) < fabs(p->vel) || v_target * p->vel < 0;
   double a = (hard_stop && decelerating) ? p->stop_dec : p->accel;
   double dv = a * p->dt;

   double v0 = p->vel;
   if (p->vel < v_target)
      p->vel = fmin(p->vel + dv, v_target);
   else if (p->vel > v_target)
      p->vel = fmax(p->vel - dv, v_target);

   /* Tích phân hình thang để vị trí liên tục */
   double d = 0.5 * (v0 + p->vel) * p->dt + p->frac;
   double whole = floor(d);
   p->pos += (int64_t)whole;
   p->frac = d - whole;
}
