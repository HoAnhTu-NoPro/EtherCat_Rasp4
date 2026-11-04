#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_defaults(config_t *c)
{
   memset(c, 0, sizeof(*c));
   strcpy(c->ifname, "eth0");
   c->cycle_us = 1000;
   c->slave = 1;
   c->pulses_per_rev = 50000;
   c->peak_current_ma = 1000;
   c->max_jog_rpm = 60;
   c->accel_rpm_s = 300;
   c->stop_decel_rpm_s = 600;
   c->heartbeat_timeout_ms = 200;
   strcpy(c->ipc_group, "root");
}

static char *trim(char *s)
{
   while (isspace((unsigned char)*s))
      s++;
   char *e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = '\0';
   return s;
}

static void apply(config_t *c, const char *sec, const char *key, const char *val)
{
#define IS(s, k) (strcmp(sec, s) == 0 && strcmp(key, k) == 0)
   if (IS("bus", "ifname"))
      snprintf(c->ifname, sizeof(c->ifname), "%s", val);
   else if (IS("bus", "cycle_us"))
      c->cycle_us = (uint32_t)strtoul(val, NULL, 10);
   else if (IS("axis", "slave"))
      c->slave = atoi(val);
   else if (IS("axis", "pulses_per_rev"))
      c->pulses_per_rev = (int32_t)strtol(val, NULL, 10);
   else if (IS("axis", "peak_current_ma"))
      c->peak_current_ma = (uint32_t)strtoul(val, NULL, 10);
   else if (IS("limits", "max_jog_rpm"))
      c->max_jog_rpm = strtod(val, NULL);
   else if (IS("limits", "accel_rpm_s"))
      c->accel_rpm_s = strtod(val, NULL);
   else if (IS("limits", "stop_decel_rpm_s"))
      c->stop_decel_rpm_s = strtod(val, NULL);
   else if (IS("safety", "heartbeat_timeout_ms"))
      c->heartbeat_timeout_ms = (uint32_t)strtoul(val, NULL, 10);
   else if (IS("ipc", "group"))
      snprintf(c->ipc_group, sizeof(c->ipc_group), "%s", val);
   else
      fprintf(stderr, "config: bỏ qua khóa lạ [%s] %s\n", sec, key);
#undef IS
}

int config_load(const char *path, config_t *cfg)
{
   set_defaults(cfg);
   FILE *f = fopen(path, "r");
   if (!f)
   {
      perror(path);
      return -1;
   }

   char line[256], sec[32] = "";
   while (fgets(line, sizeof(line), f))
   {
      char *p = trim(line);
      if (*p == '#' || *p == ';' || *p == '\0')
         continue;
      if (*p == '[')
      {
         char *e = strchr(p, ']');
         if (e)
         {
            *e = '\0';
            snprintf(sec, sizeof(sec), "%s", trim(p + 1));
         }
         continue;
      }
      char *eq = strchr(p, '=');
      if (!eq)
         continue;
      *eq = '\0';
      apply(cfg, sec, trim(p), trim(eq + 1));
   }
   fclose(f);

   if (cfg->cycle_us < 250 || cfg->cycle_us > 10000 || cfg->pulses_per_rev <= 0 ||
       cfg->max_jog_rpm <= 0 || cfg->accel_rpm_s <= 0 || cfg->stop_decel_rpm_s <= 0 ||
       cfg->heartbeat_timeout_ms == 0 || cfg->heartbeat_timeout_ms > 1000 || cfg->slave < 1 ||
       cfg->peak_current_ma < 100 || cfg->peak_current_ma > 5600)
   {
      fprintf(stderr, "config: giá trị không hợp lệ trong %s\n", path);
      return -1;
   }
   return 0;
}

void config_print(const config_t *c)
{
   printf("config: if=%s cycle=%uus slave=%d ppr=%d peak=%umA vmax=%.0frpm acc=%.0frpm/s "
          "stop=%.0frpm/s hb=%ums group=%s\n",
          c->ifname, c->cycle_us, c->slave, c->pulses_per_rev, c->peak_current_ma, c->max_jog_rpm,
          c->accel_rpm_s, c->stop_decel_rpm_s, c->heartbeat_timeout_ms, c->ipc_group);
}
