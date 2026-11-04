#ifndef IPC_H
#define IPC_H

#include <stdbool.h>
#include <sys/types.h>

#include "ipc_layout.h"

typedef struct
{
   int sock;
   ipc_shm_t *shm;
} ipc_t;

/* Tạo shm trạng thái và socket lệnh. Socket thuộc nhóm gid, quyền 0660. */
int ipc_open(ipc_t *ipc, gid_t gid);
void ipc_close(ipc_t *ipc);

/* Đọc một lệnh không chặn. Trả về true nếu có lệnh hợp lệ. */
bool ipc_recv(ipc_t *ipc, ipc_cmd_t *cmd);

/* Ghi một khối có seqlock (seq là 4 byte đầu). src->seq bị bỏ qua. */
void ipc_publish(void *dst_block, const void *src_block, size_t size);

#endif
