#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_DIR "/run/ecat-rt"

int ipc_open(ipc_t *ipc, gid_t gid)
{
   ipc->sock = -1;
   ipc->shm = NULL;

   /* Trạng thái: ai cũng đọc được, chỉ ecat-rt ghi. */
   int fd = shm_open(IPC_SHM_NAME, O_CREAT | O_RDWR, 0644);
   if (fd < 0 || ftruncate(fd, sizeof(ipc_shm_t)) < 0)
   {
      perror("ipc: shm_open");
      return -1;
   }
   fchmod(fd, 0644);
   ipc->shm = mmap(NULL, sizeof(ipc_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   if (ipc->shm == MAP_FAILED)
   {
      ipc->shm = NULL;
      perror("ipc: mmap");
      return -1;
   }
   memset(ipc->shm, 0, sizeof(ipc_shm_t));
   ipc->shm->version = IPC_VERSION;
   ipc->shm->pid = (uint32_t)getpid();
   __atomic_store_n(&ipc->shm->magic, IPC_MAGIC, __ATOMIC_RELEASE);

   /* Lệnh: chỉ root và nhóm ipc được gửi. */
   if (mkdir(IPC_DIR, 0750) < 0 && errno != EEXIST)
   {
      perror("ipc: mkdir " IPC_DIR);
      return -1;
   }
   if (chown(IPC_DIR, 0, gid) < 0 || chmod(IPC_DIR, 0750) < 0)
      perror("ipc: chown " IPC_DIR);

   ipc->sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
   if (ipc->sock < 0)
   {
      perror("ipc: socket");
      return -1;
   }
   struct sockaddr_un addr = {.sun_family = AF_UNIX};
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", IPC_CMD_SOCKET);
   unlink(IPC_CMD_SOCKET);
   if (bind(ipc->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
   {
      perror("ipc: bind " IPC_CMD_SOCKET);
      return -1;
   }
   if (chown(IPC_CMD_SOCKET, 0, gid) < 0 || chmod(IPC_CMD_SOCKET, 0660) < 0)
      perror("ipc: chown " IPC_CMD_SOCKET);
   return 0;
}

void ipc_close(ipc_t *ipc)
{
   if (ipc->sock >= 0)
   {
      close(ipc->sock);
      unlink(IPC_CMD_SOCKET);
   }
   if (ipc->shm)
   {
      ipc->shm->rt.master_state = MS_STOPPED;
      munmap(ipc->shm, sizeof(ipc_shm_t));
   }
   /* Giữ shm để web thấy MS_STOPPED; lần chạy sau ghi đè. */
}

bool ipc_recv(ipc_t *ipc, ipc_cmd_t *cmd)
{
   ssize_t n = recv(ipc->sock, cmd, sizeof(*cmd), MSG_DONTWAIT);
   return n == (ssize_t)sizeof(*cmd) && cmd->magic == IPC_MAGIC;
}

void ipc_publish(void *dst_block, const void *src_block, size_t size)
{
   uint32_t *seq = (uint32_t *)dst_block;
   uint32_t s = __atomic_load_n(seq, __ATOMIC_RELAXED);
   __atomic_store_n(seq, s + 1, __ATOMIC_RELAXED); /* lẻ: đang ghi */
   __atomic_thread_fence(__ATOMIC_RELEASE);
   memcpy((uint8_t *)dst_block + 4, (const uint8_t *)src_block + 4, size - 4);
   __atomic_store_n(seq, s + 2, __ATOMIC_RELEASE);
}
