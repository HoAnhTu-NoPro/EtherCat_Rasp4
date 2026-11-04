/* In kích thước/offset các struct IPC để web/server/ipc.py tự kiểm tra layout. */
#include <stddef.h>
#include <stdio.h>

#include "ipc_layout.h"

int main(void)
{
   printf("rt %zu\nslave %zu\nbus %zu\nshm %zu\ncmd %zu\nrt_off %zu\nbus_off %zu\n",
          sizeof(ipc_rt_block_t), sizeof(ipc_slave_t), sizeof(ipc_bus_block_t),
          sizeof(ipc_shm_t), sizeof(ipc_cmd_t), offsetof(ipc_shm_t, rt), offsetof(ipc_shm_t, bus));
   return 0;
}
