/*
 * ecat-rt: tiến trình thời gian thực điều khiển trục DM3E qua EtherCAT.
 *
 * Bước 2: vòng lặp chu kỳ đồng bộ DC, đưa slave lên OP, đo chu kỳ/jitter.
 * Controlword luôn là 0 (trục disable), target position bám theo vị trí thực tế.
 *
 *   sudo ./ecat-rt [-c config/axis.ini] [-t giây] [-C cpu] [-p prio]
 */
#define _GNU_SOURCE
#include <errno.h>
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
#include "config.h"

#define NSEC_PER_SEC 1000000000LL

typedef struct
{
   /* Cửa sổ 1 giây gần nhất */
   int64_t period_min_ns, period_max_ns, period_avg_ns;
   int64_t lat_max_ns;
   /* Toàn bộ thời gian chạy */
   int64_t lat_max_total_ns;
   uint64_t lat_over[4]; /* > 50, 100, 200, 500 µs */
   uint64_t cycles;
   uint64_t wkc_errors;
   int64_t dc_error_ns;
   int wkc;
   uint16_t statusword, error_code;
   int8_t mode;
   int32_t position;
} rt_stats_t;

static const int64_t lat_thresholds_ns[4] = {50000, 100000, 200000, 500000};

static bus_t bus;
static config_t cfg;
static atomic_int run_rt = 1;
static volatile sig_atomic_t stop_req = 0;

/* seqlock: RT ghi, thread khác đọc, RT không bao giờ chờ. */
static atomic_uint stats_seq;
static rt_stats_t stats_pub;

static void stats_publish(const rt_stats_t *s)
{
   atomic_fetch_add_explicit(&stats_seq, 1, memory_order_release);
   atomic_thread_fence(memory_order_release);
   stats_pub = *s;
   atomic_thread_fence(memory_order_release);
   atomic_fetch_add_explicit(&stats_seq, 1, memory_order_release);
}

static void stats_read(rt_stats_t *out)
{
   unsigned s0, s1;
   do
   {
      s0 = atomic_load_explicit(&stats_seq, memory_order_acquire);
      atomic_thread_fence(memory_order_acquire);
      *out = stats_pub;
      atomic_thread_fence(memory_order_acquire);
      s1 = atomic_load_explicit(&stats_seq, memory_order_acquire);
   } while ((s0 & 1) || s0 != s1);
}

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
   const uint64_t window = NSEC_PER_SEC / cycle;
   rt_stats_t st;
   memset(&st, 0, sizeof(st));

   int64_t toff = 0, w_min = INT64_MAX, w_max = 0, w_sum = 0, w_lat = 0;
   uint64_t w_n = 0;

   struct timespec next, now;
   clock_gettime(CLOCK_MONOTONIC, &next);
   next.tv_nsec = (next.tv_nsec / 1000000 + 1) * 1000000; /* tròn ms */
   if (next.tv_nsec >= NSEC_PER_SEC)
   {
      next.tv_sec++;
      next.tv_nsec -= NSEC_PER_SEC;
   }
   int64_t prev = 0;

   ecx_send_processdata(&bus.ctx);
   while (atomic_load_explicit(&run_rt, memory_order_relaxed))
   {
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
      if (lat > st.lat_max_total_ns) st.lat_max_total_ns = lat;
      for (int i = 0; i < 4; i++)
         if (lat > lat_thresholds_ns[i])
            st.lat_over[i]++;

      st.wkc = ecx_receive_processdata(&bus.ctx, EC_TIMEOUTRET);
      if (st.wkc != bus.expected_wkc)
         st.wkc_errors++;
      if (st.wkc > 0)
         toff = dc_sync(bus.ctx.DCtime, cycle, &st.dc_error_ns);

      /* Bước 2: luôn disable, CSP target bám vị trí thực tế để không giật khi enable sau này. */
      bus.out->controlword = 0;
      bus.out->target_position = bus.in->actual_position;

      ecx_send_processdata(&bus.ctx);

      st.cycles++;
      if (w_n >= window)
      {
         st.period_min_ns = w_min;
         st.period_max_ns = w_max;
         st.period_avg_ns = w_sum / (int64_t)w_n;
         st.lat_max_ns = w_lat;
         st.statusword = bus.in->statusword;
         st.error_code = bus.in->error_code;
         st.mode = bus.in->mode_display;
         st.position = bus.in->actual_position;
         stats_publish(&st);
         w_min = INT64_MAX;
         w_max = w_sum = w_lat = 0;
         w_n = 0;
      }
   }
   return NULL;
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

int main(int argc, char **argv)
{
   const char *cfg_path = "config/axis.ini";
   int duration_s = 0, cpu = 3, prio = 80, opt;

   while ((opt = getopt(argc, argv, "c:t:C:p:")) != -1)
   {
      switch (opt)
      {
      case 'c': cfg_path = optarg; break;
      case 't': duration_s = atoi(optarg); break;
      case 'C': cpu = atoi(optarg); break;
      case 'p': prio = atoi(optarg); break;
      default:
         fprintf(stderr, "dùng: %s [-c cfg] [-t giây] [-C cpu|-1] [-p prio]\n", argv[0]);
         return 2;
      }
   }

   if (config_load(cfg_path, &cfg))
      return 1;
   config_print(&cfg);

   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);
   if (mlockall(MCL_CURRENT | MCL_FUTURE))
      perror("mlockall");

   if (bus_open(&bus, cfg.ifname, cfg.slave, cfg.cycle_us * 1000))
   {
      bus_close(&bus);
      return 1;
   }

   pthread_t th;
   if (start_rt_thread(&th, cpu, prio))
   {
      bus_close(&bus);
      return 1;
   }

   /* Đợi DC đồng bộ ổn định rồi mới lên OP. */
   sleep(1);
   bus_request_state(&bus, EC_STATE_OPERATIONAL);
   ecx_statecheck(&bus.ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);

   printf("   t  state  per_avg  per_min  per_max  lat_max  dc_err   wkc  wkc_err  sw      err    mode  pos\n");
   for (int t = 1; !stop_req && (duration_s == 0 || t <= duration_s); t++)
   {
      sleep(1);
      uint16_t al;
      uint16_t state = bus_read_state(&bus, &al);
      rt_stats_t s;
      stats_read(&s);
      printf("%4d  %-6s %7.1f  %7.1f  %7.1f  %7.1f  %6.1f  %3d  %7llu  0x%04X  0x%04X  %3d  %d\n",
             t, bus_state_name(state), s.period_avg_ns / 1e3, s.period_min_ns / 1e3,
             s.period_max_ns / 1e3, s.lat_max_ns / 1e3, s.dc_error_ns / 1e3, s.wkc,
             (unsigned long long)s.wkc_errors, s.statusword, s.error_code, s.mode, s.position);
      if ((state & 0x0F) != EC_STATE_OPERATIONAL && al)
         printf("      AL 0x%04X: %s\n", al, ec_ALstatuscode2string(al));
      fflush(stdout);
   }

   atomic_store(&run_rt, 0);
   pthread_join(th, NULL);

   rt_stats_t s;
   stats_read(&s);
   printf("\nTổng: %llu chu kỳ, latency max %.1f µs, >50µs %llu, >100µs %llu, >200µs %llu, >500µs %llu, lỗi WKC %llu\n",
          (unsigned long long)s.cycles, s.lat_max_total_ns / 1e3,
          (unsigned long long)s.lat_over[0], (unsigned long long)s.lat_over[1],
          (unsigned long long)s.lat_over[2], (unsigned long long)s.lat_over[3],
          (unsigned long long)s.wkc_errors);

   bus_close(&bus);
   return 0;
}
