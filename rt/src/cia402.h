#ifndef CIA402_H
#define CIA402_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc_layout.h"

/* Controlword */
#define CW_DISABLE_VOLTAGE 0x0000
#define CW_SHUTDOWN 0x0006
#define CW_SWITCH_ON 0x0007
#define CW_ENABLE_OPERATION 0x000F
#define CW_FAULT_RESET 0x0080

enum ipc_axis_state cia402_decode(uint16_t statusword);
const char *cia402_state_name(enum ipc_axis_state s);

typedef struct
{
   bool enable_req;      /* giữ enable; xóa khi lỗi/mất bus/timeout */
   bool enable_timeout;  /* lần enable gần nhất thất bại */
   uint32_t enable_cycles; /* số chu kỳ đã chờ enable */
   uint32_t reset_cycles;  /* > 0: đang phát xung fault reset */
} cia402_t;

/* Gọi mỗi chu kỳ. ready_to_disable: profile đã dừng hẳn (cho phép cắt điện motor).
 * Trả về controlword cần gửi. */
uint16_t cia402_step(cia402_t *c, uint16_t statusword, bool ready_to_disable, uint32_t enable_timeout_cycles);

/* Yêu cầu từ người dùng */
void cia402_request_enable(cia402_t *c);
void cia402_request_disable(cia402_t *c);
void cia402_request_fault_reset(cia402_t *c, uint32_t pulse_cycles);
/* Mất bus hoặc lỗi: bỏ yêu cầu enable ngay, không tự enable lại. */
void cia402_force_disable(cia402_t *c);

#endif
