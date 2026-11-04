/*
 * ecat-scan: quét bus EtherCAT, in thông tin slave và một số SDO của DM3E.
 *
 * Chỉ đưa slave lên PRE-OP để đọc SDO, không map PDO, không lên OP,
 * không bao giờ ghi controlword. Kết thúc thì trả slave về INIT.
 *
 *   sudo ./ecat-scan [ifname]      (mặc định eth0)
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#include "soem/soem.h"

static ecx_contextt ctx;

static const char *state_name(uint16 s)
{
   switch (s & 0x0F)
   {
   case EC_STATE_INIT:     return "INIT";
   case EC_STATE_PRE_OP:   return "PREOP";
   case EC_STATE_BOOT:     return "BOOT";
   case EC_STATE_SAFE_OP:  return "SAFEOP";
   case EC_STATE_OPERATIONAL: return "OP";
   default:                return "?";
   }
}

/* Đọc SDO số nguyên không dấu tối đa 32 bit. Trả về 0 nếu thành công. */
static int sdo_u32(uint16 slave, uint16 idx, uint8 sub, int size, uint32 *out)
{
   uint32 v = 0;
   int sz = size;
   int wkc = ecx_SDOread(&ctx, slave, idx, sub, FALSE, &sz, &v, EC_TIMEOUTRXM);
   if (wkc <= 0)
      return -1;
   *out = v;
   return 0;
}

static void print_u(uint16 slave, uint16 idx, uint8 sub, int size, const char *label)
{
   uint32 v;
   if (sdo_u32(slave, idx, sub, size, &v) == 0)
      printf("  %-28s 0x%04X:%02X = %" PRIu32 " (0x%" PRIX32 ")\n", label, idx, sub, v, v);
   else
      printf("  %-28s 0x%04X:%02X = <không đọc được>\n", label, idx, sub);
}

static void print_str(uint16 slave, uint16 idx, const char *label)
{
   char buf[64] = {0};
   int sz = sizeof(buf) - 1;
   if (ecx_SDOread(&ctx, slave, idx, 0, FALSE, &sz, buf, EC_TIMEOUTRXM) > 0)
      printf("  %-28s 0x%04X    = \"%s\"\n", label, idx, buf);
   else
      printf("  %-28s 0x%04X    = <không đọc được>\n", label, idx);
}

/* In PDO assign (1C12/1C13) và nội dung từng PDO mapping. */
static void print_pdo_assign(uint16 slave, uint16 assign_idx)
{
   uint32 n;
   if (sdo_u32(slave, assign_idx, 0, 1, &n) != 0)
   {
      printf("  0x%04X: <không đọc được>\n", assign_idx);
      return;
   }
   for (uint8 i = 1; i <= n; i++)
   {
      uint32 pdo, cnt;
      if (sdo_u32(slave, assign_idx, i, 2, &pdo) != 0 || sdo_u32(slave, (uint16)pdo, 0, 1, &cnt) != 0)
         continue;
      printf("  0x%04X:%02X -> PDO 0x%04" PRIX32 " (%" PRIu32 " mục)\n", assign_idx, i, pdo, cnt);
      for (uint8 j = 1; j <= cnt; j++)
      {
         uint32 e;
         if (sdo_u32(slave, (uint16)pdo, j, 4, &e) == 0)
            printf("      0x%04" PRIX32 ":%02" PRIX32 " %2" PRIu32 " bit\n",
                   e >> 16, (e >> 8) & 0xFF, e & 0xFF);
      }
   }
}

static void print_dm3e(uint16 slave)
{
   printf("  -- Thông tin thiết bị --\n");
   print_str(slave, 0x1008, "Tên thiết bị");
   print_str(slave, 0x1009, "Phiên bản phần cứng");
   print_str(slave, 0x100A, "Phiên bản phần mềm");
   print_u(slave, 0x3100, 1, 2, "FW driver");
   print_u(slave, 0x3100, 3, 2, "FW stack");
   print_u(slave, 0x6502, 0, 4, "Chế độ hỗ trợ (bitmask)");
   printf("  -- Trạng thái --\n");
   print_u(slave, 0x6041, 0, 2, "Statusword");
   print_u(slave, 0x603F, 0, 2, "Mã lỗi 603F");
   print_u(slave, 0x3FFE, 1, 2, "Cảnh báo gần nhất 3FFE");
   print_u(slave, 0x6061, 0, 1, "Chế độ đang chạy 6061");
   print_u(slave, 0x6064, 0, 4, "Vị trí thực tế");
   print_u(slave, 0x2048, 0, 2, "Điện áp bus DC [V]");
   printf("  -- Tham số động cơ --\n");
   print_u(slave, 0x2000, 0, 2, "Dòng đỉnh [mA]");
   print_u(slave, 0x2001, 0, 2, "Xung/vòng");
   print_u(slave, 0x2003, 0, 2, "Dòng chờ [%]");
   print_u(slave, 0x6080, 0, 4, "Tốc độ tối đa [rpm]");
   print_u(slave, 0x5014, 0, 2, "Giới hạn quá tốc [rpm]");
   print_u(slave, 0x605A, 0, 2, "Mã dừng nhanh 605A");
   print_u(slave, 0x6085, 0, 4, "Giảm tốc dừng nhanh [P/s2]");
   printf("  -- PDO --\n");
   print_pdo_assign(slave, 0x1C12);
   print_pdo_assign(slave, 0x1C13);
}

int main(int argc, char **argv)
{
   const char *ifname = argc > 1 ? argv[1] : "eth0";

   /* Quét bus sẽ đưa slave về INIT: không được chạy khi ecat-rt đang điều khiển. */
   if (access("/run/ecat-rt/cmd.sock", F_OK) == 0)
   {
      fprintf(stderr, "ecat-rt đang chạy (/run/ecat-rt/cmd.sock tồn tại). Dừng nó trước khi quét bus (nếu ecat-rt đã chết: sudo rm /run/ecat-rt/cmd.sock).\n");
      return 3;
   }

   if (!ecx_init(&ctx, ifname))
   {
      fprintf(stderr, "ecx_init(%s) thất bại. Chạy bằng sudo và kiểm tra eth0 đã UP.\n", ifname);
      return 1;
   }

   int n = ecx_config_init(&ctx);
   if (n <= 0)
   {
      fprintf(stderr, "Không tìm thấy slave nào trên %s.\n", ifname);
      ecx_close(&ctx);
      return 2;
   }

   ecx_statecheck(&ctx, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
   ecx_readstate(&ctx);
   printf("Tìm thấy %d slave trên %s\n", ctx.slavecount, ifname);

   for (uint16 i = 1; i <= ctx.slavecount; i++)
   {
      ec_slavet *s = &ctx.slavelist[i];
      printf("\nSlave %u: %s\n", i, s->name);
      printf("  Vendor 0x%08" PRIX32 "  Product 0x%08" PRIX32 "  Rev 0x%08" PRIX32 "\n",
             s->eep_man, s->eep_id, s->eep_rev);
      printf("  Trạng thái %s (0x%02X)  AL status 0x%04X  DC %s  Addr 0x%04X\n",
             state_name(s->state), s->state, s->ALstatuscode,
             s->hasdc ? "có" : "không", s->configadr);
      if (s->mbx_proto & ECT_MBXPROT_COE)
         print_dm3e(i);
   }

   ctx.slavelist[0].state = EC_STATE_INIT;
   ecx_writestate(&ctx, 0);
   ecx_close(&ctx);
   return 0;
}
