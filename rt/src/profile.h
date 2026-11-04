#ifndef PROFILE_H
#define PROFILE_H

#include <stdbool.h>
#include <stdint.h>

/* Bộ sinh profile vận tốc hình thang cho CSP.
 * Đơn vị nội bộ: xung và xung/giây. Vị trí tích lũy int64 + phần lẻ
 * để không trôi khi chạy lâu. */
typedef struct
{
   int64_t pos;     /* xung nguyên */
   double frac;     /* phần lẻ [0,1) */
   double vel;      /* xung/s hiện tại */
   double vmax;     /* xung/s, giới hạn tuyệt đối từ cấu hình */
   double accel;    /* xung/s² */
   double stop_dec; /* xung/s² */
   double dt;       /* s */
} profile_t;

void profile_init(profile_t *p, double vmax, double accel, double stop_dec, double dt);

/* Đặt lại vị trí theo vị trí thực tế, vận tốc = 0 (khi chưa enable). */
void profile_sync(profile_t *p, int32_t actual);

/* Một chu kỳ: tiến vận tốc về v_target (bị kẹp ±vmax).
 * hard_stop: dùng gia tốc dừng (STOP, mất heartbeat, disable). */
void profile_step(profile_t *p, double v_target, bool hard_stop);

static inline int32_t profile_position(const profile_t *p)
{
   return (int32_t)(uint32_t)p->pos; /* quấn vòng như 607A 32 bit */
}

static inline bool profile_stopped(const profile_t *p)
{
   return p->vel == 0.0;
}

#endif
