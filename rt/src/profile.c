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

/* Cộng quãng đường (xung, có phần lẻ) vào vị trí tích lũy. */
static void advance(profile_t *p, double inc)
{
   double d = inc + p->frac;
   double whole = floor(d);
   p->pos += (int64_t)whole;
   p->frac = d - whole;
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

   advance(p, 0.5 * (v0 + p->vel) * p->dt);
}

bool profile_step_to(profile_t *p, int64_t target, double vmax_move)
{
   const double dv = p->accel * p->dt;
   double d = (double)(target - p->pos) - p->frac;

   if (fabs(d) < 0.5 && fabs(p->vel) <= dv)
   {
      p->pos = target;
      p->frac = 0;
      p->vel = 0;
      return true;
   }

   /* Làm việc theo hướng về đích: u0 > 0 là đang đi về phía target.
    * Chọn u lớn nhất sao cho sau chu kỳ này vẫn dừng kịp với gia tốc accel,
    * với tích phân hình thang: |d| - (u0 + u) dt/2 >= u² / (2a). */
   double sgn = d > 0 ? 1.0 : -1.0;
   double u0 = p->vel * sgn;
   double h = dv / 2;
   double rem = fabs(d) - 0.5 * u0 * p->dt;
   double u_allow = rem > 0 ? -h + sqrt(h * h + 2 * p->accel * rem) : 0;
   double vmax = fmin(vmax_move, p->vmax);
   double v_des = sgn * fmin(vmax, u_allow);

   double v0 = p->vel;
   if (p->vel < v_des)
      p->vel = fmin(p->vel + dv, v_des);
   else if (p->vel > v_des)
      p->vel = fmax(p->vel - dv, v_des);

   double inc = 0.5 * (v0 + p->vel) * p->dt;
   if (inc * d > 0 && fabs(inc) > fabs(d))
      inc = d; /* không bước qua đích */
   advance(p, inc);
   return false;
}
