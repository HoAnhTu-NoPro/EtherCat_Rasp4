/*
 * ecat-rt: tiến trình thời gian thực điều khiển trục DM3E qua EtherCAT.
 *
 * Thread RT (SCHED_FIFO, ghim CPU): mỗi chu kỳ trao đổi PDO, đồng bộ DC,
 * đọc lệnh từ Unix socket (non-blocking), chạy máy trạng thái CiA 402,
 * profile vận tốc CSP và heartbeat jog, ghi trạng thái ra shm.
 * Thread chính: giám sát trạng thái EtherCAT, khôi phục slave, in log.
 *
 * An toàn:
 *  - Khởi động: trục disable. Không bao giờ tự enable.
 *  - Mất bus (WKC sai liên tục hoặc slave rời OP): bỏ enable, cần bấm Enable lại.
 *  - Jog phải được gửi lại trong heartbeat_timeout_ms, nếu không trục giảm tốc dừng.
 *  - Tốc độ/gia tốc bị kẹp theo file cấu hình.
 *
 *   sudo ./ecat-rt [-c config/axis.ini] [-t giây] [-C cpu] [-p prio] [-q]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "bus.h"
#include "cia402.h"
#include "config.h"
#include "ipc.h"
#include "profile.h"

#define NSEC_PER_SEC 1000000000LL
#define WKC_LOST_CYCLES 3          /* WKC sai liên tiếp -> mất bus */
#define BUS_OK_CYCLES 100          /* WKC đúng liên tiếp để coi bus đã ổn lại */
#define ENABLE_TIMEOUT_MS 1000
#define FAULT_RESET_PULSE_MS 20
#define PUBLISH_EVERY 10           /* chu kỳ */

static bus_t bus;
static config_t cfg;
static ipc_t ipc;
static atomic_int run_rt = 1;
static atomic_bool slave_in_op = false; /* do thread giám sát cập nhật */
static volatile sig_atomic_t stop_req = 0;

static inline int64_t ts_ns(const struct timespec *t)
{
   return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec;
}

static inline void ts_add(struct timespec *t, int64_t ns)
{
   int64_t v = ts_ns(t) + ns;
   t->tv_sec = v / NSEC_PER_SEC;
   t->tv_nsec = v % NSEC_PER_SEC;
}

static int64_t now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return ts_ns(&t);
}

/* PI đồng bộ đồng hồ Linux theo DC: gửi frame lệch nửa chu kỳ so với Sync0. */
static int64_t dc_sync(int64_t dctime, int64_t cycle, int64_t *dc_err)
{
   static int64_t integral = 0;
   int64_t delta = (dctime - cycle / 2) % cycle;
   if (delta > cycle / 2)
      delta -= cycle;
   *dc_err = -delta;
   integral += *dc_err;
   return (int64_t)(*dc_err * 0.01 + integral * 0.00002);
}

static void *rt_thread(void *arg)
{
   (void)arg;
   const int64_t cycle = (int64_t)cfg.cycle_us * 1000;
   const double dt = cycle / 1e9;
   const double rpm_to_pps = cfg.pulses_per_rev / 60.0;
   const uint64_t window = NSEC_PER_SEC / cycle;
   const uint32_t enable_timeout_cycles = ENABLE_TIMEOUT_MS * 1000 / cfg.cycle_us;
   const uint32_t reset_pulse_cycles = FAULT_RESET_PULSE_MS * 1000 / cfg.cycle_us;
   const int64_t hb_timeout = (int64_t)cfg.heartbeat_timeout_ms * 1000000;

   ipc_rt_block_t st;
   memset(&st, 0, sizeof(st));
   st.cycle_us = cfg.cycle_us;
   st.expected_wkc = bus.expected_wkc;
   st.max_jog_rpm = cfg.max_jog_rpm;
   st.accel_rpm_s = cfg.accel_rpm_s;
   st.stop_decel_rpm_s = cfg.stop_decel_rpm_s;
   st.heartbeat_timeout_ms = cfg.heartbeat_timeout_ms;
   st.master_state = MS_STARTING;

   cia402_t drv = {0};
   profile_t prof;
   profile_init(&prof, cfg.max_jog_rpm * rpm_to_pps, cfg.accel_rpm_s * rpm_to_pps,
                cfg.stop_decel_rpm_s * rpm_to_pps, dt);

   double jog_target = 0;   /* xung/s */
   int64_t jog_deadline = 0; /* 0 = không có jog */
   bool hard_stop = false;
   bool hb_expired = false;
   bool stop_latched = false;
   int64_t last_jog_rx = 0;
   bool bus_ok = false;
   int wkc_bad = 0, wkc_good = 0;
   int32_t prev_pos = 0;
   double vel_filt = 0;
   bool have_prev = false;

   int64_t toff = 0, w_min = INT64_MAX, w_max = 0, w_sum = 0, w_lat = 0, dc_err = 0;
   uint64_t w_n = 0;

   struct timespec next, now;
   clock_gettime(CLOCK_MONOTONIC, &next);
   next.tv_nsec = (next.tv_nsec / 1000000 + 1) * 1000000;
   if (next.tv_nsec >= NSEC_PER_SEC)
   {
      next.tv_sec++;
      next.tv_nsec -= NSEC_PER_SEC;
   }
   int64_t prev = 0;

   ecx_send_processdata(&bus.ctx);
   while (atomic_load_explicit(&run_rt, memory_order_relaxed))
   {
      /* ---- Định thời ---- */
      ts_add(&next, cycle + toff);
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
         ;
      clock_gettime(CLOCK_MONOTONIC, &now);
      int64_t t = ts_ns(&now);
      int64_t lat = t - ts_ns(&next);
      if (prev)
      {
         int64_t p = t - prev;
         if (p < w_min) w_min = p;
         if (p > w_max) w_max = p;
         w_sum += p;
         w_n++;
      }
      prev = t;
      if (lat > w_lat) w_lat = lat;
      if (lat > (int64_t)(st.lat_max_total_us * 1000)) st.lat_max_total_us = lat / 1e3f;
      if (lat > 100000) st.lat_over_100us++;

      /* ---- Trao đổi PDO ---- */
      st.wkc = ecx_receive_processdata(&bus.ctx, EC_TIMEOUTRET);
      if (st.wkc == bus.expected_wkc)
      {
         wkc_bad = 0;
         wkc_good++;
      }
      else
      {
         st.wkc_errors++;
         wkc_good = 0;
         wkc_bad++;
      }
      if (st.wkc > 0)
         toff = dc_sync(bus.ctx.DCtime, cycle, &dc_err);

      /* ---- Sức khỏe bus ---- */
      bool in_op = atomic_load_explicit(&slave_in_op, memory_order_relaxed);
      if (bus_ok && (wkc_bad >= WKC_LOST_CYCLES || !in_op))
      {
         bus_ok = false;
         cia402_force_disable(&drv);
      }
      else if (!bus_ok && wkc_good >= BUS_OK_CYCLES && in_op)
      {
         bus_ok = true; /* bus ổn lại nhưng trục vẫn disable */
      }
      st.master_state = bus_ok ? MS_RUNNING : (st.master_state == MS_STARTING ? MS_STARTING : MS_BUS_LOST);

      uint16_t sw = bus_ok ? bus.in->statusword : 0;
      enum ipc_axis_state ax = bus_ok ? cia402_decode(sw) : AX_UNKNOWN;
      bool enabled = ax == AX_OPERATION_ENABLED;

      /* ---- Lệnh ---- */
      ipc_cmd_t cmd;
      for (int i = 0; i < 16 && ipc_recv(&ipc, &cmd); i++)
      {
         switch (cmd.type)
         {
         case CMD_ENABLE:
            if (bus_ok && ax != AX_FAULT && ax != AX_FAULT_REACTION_ACTIVE)
            {
               cia402_request_enable(&drv);
               jog_target = 0;
               jog_deadline = 0;
               hard_stop = false;
               hb_expired = false;
            }
            break;
         case CMD_DISABLE:
            cia402_request_disable(&drv);
            jog_target = 0;
            jog_deadline = 0;
            break;
         case CMD_FAULT_RESET:
            cia402_request_fault_reset(&drv, reset_pulse_cycles);
            jog_target = 0;
            jog_deadline = 0;
            break;
         case CMD_STOP:
            jog_target = 0;
            jog_deadline = 0;
            hard_stop = true;
            stop_latched = true;
            break;
         case CMD_JOG:
            last_jog_rx = t;
            if (!stop_latched && enabled && drv.enable_req && isfinite(cmd.velocity_rpm))
            {
               jog_target = cmd.velocity_rpm * rpm_to_pps; /* profile_step tự kẹp ±vmax */
               jog_deadline = t + hb_timeout;
               hard_stop = false;
               hb_expired = false;
            }
            break;
         }
      }

      /* Chốt STOP được nhả khi client đã ngừng gửi jog (người dùng nhả nút). */
      if (stop_latched && t - last_jog_rx > hb_timeout)
         stop_latched = false;

      /* ---- Heartbeat ---- */
      if (jog_deadline && t > jog_deadline)
      {
         jog_target = 0;
         jog_deadline = 0;
         hard_stop = true;
         hb_expired = true;
      }

      /* ---- Profile + CiA 402 ---- */
      if (enabled)
      {
         if (drv.enable_req)
            profile_step(&prof, jog_target, hard_stop);
         else
            profile_step(&prof, 0, true); /* disable: dừng trước rồi mới cắt */
         if (hard_stop && profile_stopped(&prof))
            hard_stop = false;
      }
      else
      {
         profile_sync(&prof, bus.in->actual_position);
         jog_target = 0;
         jog_deadline = 0;
         hard_stop = false;
      }

      uint16_t cw = bus_ok ? cia402_step(&drv, sw, profile_stopped(&prof), enable_timeout_cycles)
                           : CW_DISABLE_VOLTAGE;
      bus.out->controlword = cw;
      bus.out->target_position = enabled ? profile_position(&prof) : bus.in->actual_position;

      ecx_send_processdata(&bus.ctx);

      /* ---- Vận tốc thực tế từ 6064 (lọc thông thấp ~20 ms) ---- */
      int32_t pos = bus.in->actual_position;
      if (have_prev)
      {
         double v = (double)(int32_t)((uint32_t)pos - (uint32_t)prev_pos) / dt;
         vel_filt += (v - vel_filt) * 0.05;
      }
      prev_pos = pos;
      have_prev = true;

      /* ---- Thống kê và xuất trạng thái ---- */
      st.cycles++;
      if (w_n >= window)
      {
         st.period_min_us = w_min / 1e3f;
         st.period_max_us = w_max / 1e3f;
         st.period_avg_us = (float)(w_sum / (int64_t)w_n) / 1e3f;
         st.lat_max_us = w_lat / 1e3f;
         w_min = INT64_MAX;
         w_max = w_sum = w_lat = 0;
         w_n = 0;
      }
      if (st.cycles % PUBLISH_EVERY == 0)
      {
         st.updated_ns = t;
         st.dc_error_us = dc_err / 1e3f;
         st.axis_state = ax;
         st.statusword = sw;
         st.controlword = cw;
         st.error_code = bus_ok ? bus.in->error_code : 0;
         st.mode_display = bus_ok ? bus.in->mode_display : 0;
         st.actual_position = pos;
         st.target_position = bus.out->target_position;
         st.actual_rpm = (float)(vel_filt / rpm_to_pps);
         st.profile_rpm = (float)(prof.vel / rpm_to_pps);
         st.target_rpm = (float)(fmax(-prof.vmax, fmin(prof.vmax, jog_target)) / rpm_to_pps);
         st.hb_age_ms = jog_deadline ? (uint32_t)((t - (jog_deadline - hb_timeout)) / 1000000) : 0;
         st.flags = (drv.enable_req ? FL_ENABLE_REQ : 0) | (jog_deadline ? FL_JOG_ACTIVE : 0) |
                    (hb_expired ? FL_HB_EXPIRED : 0) | (drv.enable_timeout ? FL_ENABLE_TIMEOUT : 0) |
                    (hard_stop || (enabled && !drv.enable_req) ? FL_STOPPING : 0) |
                    (stop_latched ? FL_STOP_LATCHED : 0);
         ipc_publish(&ipc.shm->rt, &st, sizeof(st));
      }
   }

   /* Thoát: cắt điện motor */
   bus.out->controlword = CW_DISABLE_VOLTAGE;
   ecx_send_processdata(&bus.ctx);
   ecx_receive_processdata(&bus.ctx, EC_TIMEOUTRET);
   st.master_state = MS_STOPPED;
   st.updated_ns = now_ns();
   ipc_publish(&ipc.shm->rt, &st, sizeof(st));
   return NULL;
}

static void publish_bus(void)
{
   ipc_bus_block_t b;
   memset(&b, 0, sizeof(b));
   b.updated_ns = now_ns();
   snprintf(b.ifname, sizeof(b.ifname), "%s", cfg.ifname);
   b.slave_count = bus.ctx.slavecount;
   for (int i = 1; i <= bus.ctx.slavecount && i <= IPC_MAX_SLAVES; i++)
   {
      ec_slavet *s = &bus.ctx.slavelist[i];
      ipc_slave_t *o = &b.slaves[i - 1];
      o->vendor = s->eep_man;
      o->product = s->eep_id;
      o->revision = s->eep_rev;
      o->state = s->state;
      o->al_status_code = s->ALstatuscode;
      snprintf(o->name, sizeof(o->name), "%s", s->name);
   }
   ipc_publish(&ipc.shm->bus, &b, sizeof(b));
}

static void on_signal(int sig)
{
   (void)sig;
   stop_req = 1;
}

static int start_rt_thread(pthread_t *th, int cpu, int prio)
{
   pthread_attr_t a;
   pthread_attr_init(&a);
   pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
   pthread_attr_setschedpolicy(&a, SCHED_FIFO);
   struct sched_param sp = {.sched_priority = prio};
   pthread_attr_setschedparam(&a, &sp);
   if (cpu >= 0)
   {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(cpu, &set);
      pthread_attr_setaffinity_np(&a, sizeof(set), &set);
   }
   int r = pthread_create(th, &a, rt_thread, NULL);
   pthread_attr_destroy(&a);
   if (r)
      fprintf(stderr, "pthread_create: %s (cần root cho SCHED_FIFO)\n", strerror(r));
   return r;
}

static void print_status(int t)
{
   ipc_rt_block_t s = ipc.shm->rt; /* chỉ để log, không cần seqlock chặt */
   static const char *ms[] = {"START", "RUN", "LOST", "STOP"};
   printf("%5d %-5s %-6s per %6.1f/%6.1f lat %5.1f wkc_err %-4llu sw 0x%04X cw 0x%04X err 0x%04X "
          "%-18s pos %-9d v %7.1f/%7.1f/%7.1f rpm fl 0x%02X\n",
          t, ms[s.master_state & 3], bus_state_name(bus.ctx.slavelist[cfg.slave].state),
          s.period_min_us, s.period_max_us, s.lat_max_us, (unsigned long long)s.wkc_errors,
          s.statusword, s.controlword, s.error_code, cia402_state_name(s.axis_state),
          s.actual_position, s.target_rpm, s.profile_rpm, s.actual_rpm, s.flags);
   fflush(stdout);
}

int main(int argc, char **argv)
{
   const char *cfg_path = "config/axis.ini";
   int duration_s = 0, cpu = 3, prio = 80, quiet = 0, opt;

   while ((opt = getopt(argc, argv, "c:t:C:p:q")) != -1)
   {
      switch (opt)
      {
      case 'c': cfg_path = optarg; break;
      case 't': duration_s = atoi(optarg); break;
      case 'C': cpu = atoi(optarg); break;
      case 'p': prio = atoi(optarg); break;
      case 'q': quiet = 1; break;
      default:
         fprintf(stderr, "dùng: %s [-c cfg] [-t giây] [-C cpu|-1] [-p prio] [-q]\n", argv[0]);
         return 2;
      }
   }

   if (config_load(cfg_path, &cfg))
      return 1;
   config_print(&cfg);

   struct group *gr = getgrnam(cfg.ipc_group);
   if (!gr)
   {
      fprintf(stderr, "không có nhóm '%s' ([ipc] group)\n", cfg.ipc_group);
      return 1;
   }

   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);
   if (mlockall(MCL_CURRENT | MCL_FUTURE))
      perror("mlockall");

   if (ipc_open(&ipc, gr->gr_gid))
   {
      ipc_close(&ipc);
      return 1;
   }

   if (bus_open(&bus, &cfg))
   {
      bus_close(&bus);
      ipc_close(&ipc);
      return 1;
   }
   publish_bus();

   pthread_t th;
   if (start_rt_thread(&th, cpu, prio))
   {
      bus_close(&bus);
      ipc_close(&ipc);
      return 1;
   }

   /* Đợi DC đồng bộ ổn định rồi mới lên OP. */
   sleep(1);
   bus_request_state(&bus, EC_STATE_OPERATIONAL);
   ecx_statecheck(&bus.ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);

   /* Giám sát: 200 ms một lần */
   int64_t start = now_ns(), last_print = 0;
   while (!stop_req)
   {
      usleep(200000);
      bool op = bus_check_recover(&bus);
      atomic_store(&slave_in_op, op);
      publish_bus();

      int64_t el = (now_ns() - start) / NSEC_PER_SEC;
      if (!quiet && el != last_print)
      {
         last_print = el;
         print_status((int)el);
      }
      if (duration_s && el >= duration_s)
         break;
   }

   atomic_store(&run_rt, 0);
   pthread_join(th, NULL);
   publish_bus();
   printf("Dừng: %llu chu kỳ, latency max %.1f µs, >100µs %llu, lỗi WKC %llu\n",
          (unsigned long long)ipc.shm->rt.cycles, ipc.shm->rt.lat_max_total_us,
          (unsigned long long)ipc.shm->rt.lat_over_100us, (unsigned long long)ipc.shm->rt.wkc_errors);

   bus_close(&bus);
   ipc_close(&ipc);
   return 0;
}
