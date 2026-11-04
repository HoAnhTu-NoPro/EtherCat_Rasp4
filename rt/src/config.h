#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct
{
   char ifname[32];
   uint32_t cycle_us;

   int slave;
   int32_t pulses_per_rev;

   double max_jog_rpm;
   double accel_rpm_s;
   double stop_decel_rpm_s;

   uint32_t heartbeat_timeout_ms;
} config_t;

/* Đọc file INI. Khóa thiếu giữ giá trị mặc định an toàn. Trả về 0 nếu thành công. */
int config_load(const char *path, config_t *cfg);
void config_print(const config_t *cfg);

#endif
