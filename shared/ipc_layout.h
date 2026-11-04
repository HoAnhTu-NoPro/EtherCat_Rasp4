/*
 * Giao tiếp ecat-rt <-> web/ecat-ctl.
 *
 * Lệnh:     Unix datagram socket IPC_CMD_SOCKET, mỗi datagram một ipc_cmd_t.
 *           ecat-rt đọc non-blocking mỗi chu kỳ, không bao giờ trả lời.
 * Trạng thái: POSIX shm IPC_SHM_NAME chứa ipc_shm_t, chỉ ecat-rt ghi.
 *           Mỗi khối có seqlock riêng: đọc seq (chẵn), copy, đọc lại seq,
 *           giống nhau thì dữ liệu hợp lệ.
 *
 * Tất cả struct là packed, little-endian, chỉ dùng kiểu cố định
 * để web/server/ipc.py đọc bằng module struct. Đổi layout thì tăng IPC_VERSION.
 */
#ifndef IPC_LAYOUT_H
#define IPC_LAYOUT_H

#include <stdint.h>

#define IPC_VERSION 2
#define IPC_MAGIC 0x45434154u /* "ECAT" */
#define IPC_CMD_SOCKET "/run/ecat-rt/cmd.sock"
#define IPC_SHM_NAME "/ecat-rt-status"
#define IPC_MAX_SLAVES 4

enum ipc_cmd_type
{
   CMD_ENABLE = 1,
   CMD_DISABLE = 2,
   CMD_FAULT_RESET = 3,
   CMD_STOP = 4,      /* dừng và chốt: JOG bị bỏ qua đến khi nhả nút (ngừng gửi > heartbeat) */
   CMD_JOG = 5, /* velocity_rpm có dấu; phải gửi lại trước khi hết heartbeat_timeout_ms */
};

typedef struct __attribute__((packed))
{
   uint32_t magic;
   uint32_t type;
   float velocity_rpm;
   uint32_t seq;
} ipc_cmd_t;

/* Trạng thái trục giải mã từ statusword (CiA 402). */
enum ipc_axis_state
{
   AX_UNKNOWN = 0,
   AX_NOT_READY = 1,
   AX_SWITCH_ON_DISABLED = 2,
   AX_READY_TO_SWITCH_ON = 3,
   AX_SWITCHED_ON = 4,
   AX_OPERATION_ENABLED = 5,
   AX_QUICK_STOP_ACTIVE = 6,
   AX_FAULT_REACTION_ACTIVE = 7,
   AX_FAULT = 8,
};

enum ipc_master_state
{
   MS_STARTING = 0,
   MS_RUNNING = 1,  /* slave ở OP, WKC đúng */
   MS_BUS_LOST = 2, /* mất WKC hoặc slave rời OP: trục bị disable */
   MS_STOPPED = 3,
};

/* Cờ trong ipc_rt_block_t.flags */
#define FL_ENABLE_REQ (1u << 0)    /* người dùng yêu cầu enable */
#define FL_JOG_ACTIVE (1u << 1)    /* đang nhận lệnh jog còn hạn */
#define FL_HB_EXPIRED (1u << 2)    /* lần jog gần nhất kết thúc do mất heartbeat */
#define FL_ENABLE_TIMEOUT (1u << 3) /* enable không thành công trong 1 s */
#define FL_STOPPING (1u << 4)      /* đang giảm tốc dừng (STOP/heartbeat/disable) */
#define FL_STOP_LATCHED (1u << 5)  /* sau STOP: bỏ qua JOG đến khi client ngừng gửi jog > heartbeat */

/* Khối do thread RT ghi, cập nhật mỗi 10 ms. */
typedef struct __attribute__((packed))
{
   uint32_t seq;
   uint32_t master_state;
   int64_t updated_ns; /* CLOCK_MONOTONIC; cũ hơn ~0.5 s nghĩa là ecat-rt đã chết */
   uint64_t cycles;
   uint64_t wkc_errors;
   uint64_t lat_over_100us;
   float cycle_us;    /* chu kỳ cấu hình */
   float period_avg_us, period_min_us, period_max_us; /* cửa sổ 1 s */
   float lat_max_us;       /* cửa sổ 1 s */
   float lat_max_total_us; /* từ khi khởi động */
   float dc_error_us;
   int32_t wkc, expected_wkc;

   uint32_t axis_state;
   uint32_t flags;
   uint16_t statusword, controlword;
   uint16_t error_code; /* 0x603F */
   int16_t mode_display; /* 0x6061 */
   int32_t actual_position, target_position;
   float actual_rpm;  /* tính từ 6064 */
   float profile_rpm; /* vận tốc profile đang phát */
   float target_rpm;  /* vận tốc jog đang yêu cầu (đã kẹp) */
   uint32_t hb_age_ms;

   /* Giới hạn từ file cấu hình, để giao diện hiển thị */
   float max_jog_rpm, accel_rpm_s, stop_decel_rpm_s;
   uint32_t heartbeat_timeout_ms;

   /* Ảnh dữ liệu quá trình của chu kỳ gần nhất, đúng byte trên dây */
   int32_t target_delta; /* 607A chu kỳ này - chu kỳ trước (xung/chu kỳ) */
   uint8_t pdo_out[8];   /* RxPDO 0x1600: Pi -> driver */
   uint8_t pdo_in[19];   /* TxPDO 0x1A00: driver -> Pi */
   uint8_t reserved[5];  /* đệm: khối tiếp theo phải căn lề 8 byte */
} ipc_rt_block_t;

typedef struct __attribute__((packed))
{
   uint32_t vendor, product, revision;
   uint16_t state; /* thanh ghi AL status: 1 INIT, 2 PREOP, 4 SAFEOP, 8 OP, +0x10 lỗi */
   uint16_t al_status_code;
   char name[40];
} ipc_slave_t;

/* Khối do thread giám sát ghi, cập nhật mỗi 200 ms. */
typedef struct __attribute__((packed))
{
   uint32_t seq;
   uint32_t slave_count;
   int64_t updated_ns;
   char ifname[32];
   ipc_slave_t slaves[IPC_MAX_SLAVES];
} ipc_bus_block_t;

typedef struct __attribute__((packed))
{
   uint32_t magic;
   uint32_t version;
   uint32_t pid;
   uint32_t reserved;
   ipc_rt_block_t rt;
   ipc_bus_block_t bus;
} ipc_shm_t;

/* seq dùng lệnh atomic: trên ARM64 địa chỉ lệch căn lề gây SIGBUS. */
_Static_assert(sizeof(ipc_rt_block_t) % 8 == 0, "ipc_rt_block_t phải là bội số của 8 byte");
_Static_assert(sizeof(ipc_bus_block_t) % 8 == 0, "ipc_bus_block_t phải là bội số của 8 byte");
_Static_assert(sizeof(ipc_slave_t) % 4 == 0, "ipc_slave_t phải là bội số của 4 byte");

#endif
