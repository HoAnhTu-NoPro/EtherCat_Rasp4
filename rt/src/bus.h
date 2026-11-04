#ifndef BUS_H
#define BUS_H

#include <stdint.h>

#include "soem/soem.h"

/* PDO mặc định của DM3E-556 (0x1600 / 0x1A00), đọc thực tế bằng ecat-scan. */
typedef struct __attribute__((packed))
{
   uint16_t controlword;     /* 0x6040 */
   int32_t target_position;  /* 0x607A */
   uint16_t probe_function;  /* 0x60B8 */
} dm3e_rx_t;                 /* master -> slave, 8 byte */

typedef struct __attribute__((packed))
{
   uint16_t error_code;      /* 0x603F */
   uint16_t statusword;      /* 0x6041 */
   int8_t mode_display;      /* 0x6061 */
   int32_t actual_position;  /* 0x6064 */
   uint16_t probe_status;    /* 0x60B9 */
   int32_t probe1_pos;       /* 0x60BA */
   uint32_t digital_inputs;  /* 0x60FD */
} dm3e_tx_t;                 /* slave -> master, 19 byte */

typedef struct
{
   ecx_contextt ctx;
   uint8_t iomap[256];
   int slave;
   int expected_wkc;
   dm3e_rx_t *out;
   dm3e_tx_t *in;
} bus_t;

/* Quét bus, kiểm tra slave là DM3E, đặt CSP qua SDO, map PDO, cấu hình DC Sync0.
 * Kết thúc ở SAFE-OP-ready (PRE-OP + map xong). Trả về 0 nếu thành công. */
int bus_open(bus_t *b, const char *ifname, int slave, uint32_t cycle_ns);

/* Yêu cầu chuyển tất cả slave sang trạng thái (không chờ). */
void bus_request_state(bus_t *b, uint16_t state);

/* Đọc trạng thái slave của trục (gửi frame, không gọi từ thread RT). */
uint16_t bus_read_state(bus_t *b, uint16_t *al_status);

void bus_close(bus_t *b);

const char *bus_state_name(uint16_t s);

#endif
