#include "bus.h"

#include <stdio.h>
#include <string.h>

#define DM3E_VENDOR 0x00004321u
#define DM3E_556_PRODUCT 0x00008100u
#define MODE_CSP 8

const char *bus_state_name(uint16_t s)
{
   switch (s & 0x0F)
   {
   case EC_STATE_NONE:        return "NONE";
   case EC_STATE_INIT:        return "INIT";
   case EC_STATE_PRE_OP:      return "PREOP";
   case EC_STATE_BOOT:        return "BOOT";
   case EC_STATE_SAFE_OP:     return "SAFEOP";
   case EC_STATE_OPERATIONAL: return "OP";
   default:                   return "?";
   }
}

int bus_open(bus_t *b, const char *ifname, int slave, uint32_t cycle_ns)
{
   memset(b, 0, sizeof(*b));
   b->slave = slave;

   if (!ecx_init(&b->ctx, ifname))
   {
      fprintf(stderr, "bus: ecx_init(%s) thất bại (cần root, eth0 phải UP)\n", ifname);
      return -1;
   }
   if (ecx_config_init(&b->ctx) < slave)
   {
      fprintf(stderr, "bus: không thấy slave %d\n", slave);
      return -1;
   }

   ec_slavet *s = &b->ctx.slavelist[slave];
   if (s->eep_man != DM3E_VENDOR || s->eep_id != DM3E_556_PRODUCT)
   {
      fprintf(stderr, "bus: slave %d là %s (0x%08X/0x%08X), không phải DM3E-556\n",
              slave, s->name, s->eep_man, s->eep_id);
      return -1;
   }

   if (ecx_statecheck(&b->ctx, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE) != EC_STATE_PRE_OP)
   {
      fprintf(stderr, "bus: slave không lên được PRE-OP\n");
      return -1;
   }

   /* Đảm bảo CSP trước khi map; 6060 không có trong PDO mặc định. */
   int8_t mode = MODE_CSP;
   if (ecx_SDOwrite(&b->ctx, slave, 0x6060, 0, FALSE, sizeof(mode), &mode, EC_TIMEOUTRXM) <= 0)
   {
      fprintf(stderr, "bus: ghi 0x6060=8 thất bại\n");
      return -1;
   }

   /* Tự chuyển SAFE-OP sau khi cấu hình DC Sync0 (phải làm ở PRE-OP). */
   b->ctx.manualstatechange = 1;
   int iosize = ecx_config_map_group(&b->ctx, b->iomap, 0);
   if (iosize <= 0 || iosize > (int)sizeof(b->iomap))
   {
      fprintf(stderr, "bus: map PDO thất bại (%d byte)\n", iosize);
      return -1;
   }
   if (s->Obits != 8 * sizeof(dm3e_rx_t) || s->Ibits != 8 * sizeof(dm3e_tx_t))
   {
      fprintf(stderr, "bus: PDO không khớp mapping mặc định (out %u bit, in %u bit)\n",
              s->Obits, s->Ibits);
      return -1;
   }
   b->out = (dm3e_rx_t *)s->outputs;
   b->in = (dm3e_tx_t *)s->inputs;
   memset(b->out, 0, sizeof(*b->out)); /* controlword 0: luôn disable */

   ec_groupt *g = &b->ctx.grouplist[0];
   b->expected_wkc = g->outputsWKC * 2 + g->inputsWKC;

   if (!ecx_configdc(&b->ctx) || !s->hasdc)
   {
      fprintf(stderr, "bus: slave không hỗ trợ DC\n");
      return -1;
   }
   /* ESI: AssignActivate 0x0300 = chỉ Sync0. */
   ecx_dcsync0(&b->ctx, slave, TRUE, cycle_ns, 0);

   bus_request_state(b, EC_STATE_SAFE_OP);
   if (ecx_statecheck(&b->ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4) != EC_STATE_SAFE_OP)
   {
      uint16_t al;
      uint16_t st = bus_read_state(b, &al);
      fprintf(stderr, "bus: không lên được SAFE-OP (%s, AL 0x%04X: %s)\n",
              bus_state_name(st), al, ec_ALstatuscode2string(al));
      return -1;
   }
   printf("bus: %s ở SAFE-OP, %d byte IO, WKC mong đợi %d\n", s->name, iosize, b->expected_wkc);
   return 0;
}

void bus_request_state(bus_t *b, uint16_t state)
{
   b->ctx.slavelist[0].state = state;
   ecx_writestate(&b->ctx, 0);
}

uint16_t bus_read_state(bus_t *b, uint16_t *al_status)
{
   ecx_readstate(&b->ctx);
   ec_slavet *s = &b->ctx.slavelist[b->slave];
   if (al_status)
      *al_status = s->ALstatuscode;
   return s->state;
}

void bus_close(bus_t *b)
{
   if (b->out)
      memset(b->out, 0, sizeof(*b->out));
   bus_request_state(b, EC_STATE_INIT);
   ecx_close(&b->ctx);
}
