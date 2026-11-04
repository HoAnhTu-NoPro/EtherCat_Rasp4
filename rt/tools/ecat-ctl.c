/*
 * ecat-ctl: gửi lệnh tới ecat-rt và đọc trạng thái từ shm (để test không cần web).
 *
 *   ecat-ctl status | watch
 *   ecat-ctl enable | disable | reset | stop
 *   ecat-ctl zero <rpm> [giây_ngừng]
 *       chạy về vị trí 0, gửi heartbeat mỗi 50 ms đến khi tới nơi.
 *       giây_ngừng: ngừng gửi heartbeat sau ngần ấy giây -> test mất kết nối.
 *   ecat-ctl jog <rpm> <giây> [nohb]
 *       gửi JOG mỗi 50 ms trong <giây>, rồi STOP.
 *       nohb: hết thời gian thì ngừng gửi, KHÔNG gửi STOP -> test heartbeat.
 *
 * Cần thuộc nhóm [ipc] group (hoặc root) để gửi lệnh.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ipc_layout.h"

static const char *axis_names[] = {
   "Unknown", "Not ready", "Switch on disabled", "Ready to switch on", "Switched on",
   "Enabled", "Quick stop", "Fault reaction", "Fault"};
static const char *master_names[] = {"STARTING", "RUNNING", "BUS_LOST", "STOPPED"};

static const ipc_shm_t *shm;

static int open_shm(void)
{
   int fd = shm_open(IPC_SHM_NAME, O_RDONLY, 0);
   if (fd < 0)
   {
      perror("shm_open " IPC_SHM_NAME " (ecat-rt chưa chạy?)");
      return -1;
   }
   shm = mmap(NULL, sizeof(ipc_shm_t), PROT_READ, MAP_SHARED, fd, 0);
   close(fd);
   if (shm == MAP_FAILED || shm->magic != IPC_MAGIC || shm->version != IPC_VERSION)
   {
      fprintf(stderr, "shm không hợp lệ hoặc khác phiên bản\n");
      return -1;
   }
   return 0;
}

static void read_block(const void *src, void *dst, size_t size)
{
   const volatile uint32_t *seq = (const volatile uint32_t *)src;
   uint32_t s0, s1;
   do
   {
      s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
      memcpy(dst, src, size);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      s1 = __atomic_load_n(seq, __ATOMIC_RELAXED);
   } while ((s0 & 1) || s0 != s1);
}

static void print_status(void)
{
   ipc_rt_block_t r;
   read_block(&shm->rt, &r, sizeof(r));
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   double age = ((int64_t)t.tv_sec * 1000000000LL + t.tv_nsec - r.updated_ns) / 1e6;

   printf("%-8s%s ax=%-18s sw=0x%04X cw=0x%04X err=0x%04X mode=%d pos=%d "
          "v(tgt/prof/act)=%.1f/%.1f/%.1f rpm hb=%ums fl=0x%02X lat=%.0fus wkc_err=%llu\n",
          master_names[r.master_state & 3], age > 500 ? "(CŨ!)" : "",
          axis_names[r.axis_state < 9 ? r.axis_state : 0], r.statusword, r.controlword,
          r.error_code, r.mode_display, r.actual_position, r.target_rpm, r.profile_rpm,
          r.actual_rpm, r.hb_age_ms, r.flags, r.lat_max_us, (unsigned long long)r.wkc_errors);
}

static int sock = -1;
static struct sockaddr_un addr = {.sun_family = AF_UNIX};

static int send_cmd(uint32_t type, float rpm)
{
   static uint32_t seq;
   ipc_cmd_t c = {.magic = IPC_MAGIC, .type = type, .velocity_rpm = rpm, .seq = ++seq};
   if (sendto(sock, &c, sizeof(c), 0, (struct sockaddr *)&addr, sizeof(addr)) != sizeof(c))
   {
      perror("sendto " IPC_CMD_SOCKET);
      return -1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "dùng: %s status|watch|enable|disable|reset|stop|jog <rpm> <giây> [nohb]|zero <rpm> [giây_ngừng]\n", argv[0]);
      return 2;
   }
   if (open_shm())
      return 1;

   const char *c = argv[1];
   if (!strcmp(c, "status"))
   {
      print_status();
      return 0;
   }
   if (!strcmp(c, "watch"))
   {
      for (;;)
      {
         print_status();
         usleep(100000);
      }
   }

   sock = socket(AF_UNIX, SOCK_DGRAM, 0);
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", IPC_CMD_SOCKET);

   int r;
   if (!strcmp(c, "enable"))       r = send_cmd(CMD_ENABLE, 0);
   else if (!strcmp(c, "disable")) r = send_cmd(CMD_DISABLE, 0);
   else if (!strcmp(c, "reset"))   r = send_cmd(CMD_FAULT_RESET, 0);
   else if (!strcmp(c, "stop"))    r = send_cmd(CMD_STOP, 0);
   else if (!strcmp(c, "zero") && argc >= 3)
   {
      float rpm = strtof(argv[2], NULL);
      double cut = argc >= 4 ? strtod(argv[3], NULL) : 0;
      ipc_rt_block_t rb;
      for (int i = 0;; i++)
      {
         if (cut > 0 && i * 0.05 >= cut)
         {
            printf("-- ngừng gửi heartbeat (test mất kết nối) --\n");
            break;
         }
         if (send_cmd(CMD_GOTO_ZERO, rpm))
            return 1;
         usleep(50000);
         read_block(&shm->rt, &rb, sizeof(rb));
         if (i % 10 == 0)
            print_status();
         if (i >= 4 && !(rb.flags & FL_MOVING))
            break;
      }
      for (int i = 0; i < 6; i++)
      {
         print_status();
         usleep(100000);
      }
      read_block(&shm->rt, &rb, sizeof(rb));
      printf("%s\n", rb.flags & FL_MOVE_DONE ? "Đã về 0." : "Không về tới 0 (bị dừng, chưa enable, hoặc trục đang chạy).");
      return 0;
   }
   else if (!strcmp(c, "jog") && argc >= 4)
   {
      float rpm = strtof(argv[2], NULL);
      double secs = strtod(argv[3], NULL);
      int nohb = argc >= 5 && !strcmp(argv[4], "nohb");
      int n = (int)(secs * 20);
      for (int i = 0; i < n; i++)
      {
         if (send_cmd(CMD_JOG, rpm))
            return 1;
         if (i % 5 == 0)
            print_status();
         usleep(50000);
      }
      if (!nohb)
         send_cmd(CMD_STOP, 0);
      else
         printf("-- ngừng gửi jog (test heartbeat) --\n");
      for (int i = 0; i < 15; i++)
      {
         print_status();
         usleep(100000);
      }
      return 0;
   }
   else
   {
      fprintf(stderr, "lệnh không hợp lệ\n");
      return 2;
   }
   if (r == 0)
   {
      usleep(300000);
      print_status();
   }
   return r ? 1 : 0;
}
