#include "cia402.h"

enum ipc_axis_state cia402_decode(uint16_t sw)
{
   if ((sw & 0x4F) == 0x00) return AX_NOT_READY;
   if ((sw & 0x4F) == 0x40) return AX_SWITCH_ON_DISABLED;
   if ((sw & 0x6F) == 0x21) return AX_READY_TO_SWITCH_ON;
   if ((sw & 0x6F) == 0x23) return AX_SWITCHED_ON;
   if ((sw & 0x6F) == 0x27) return AX_OPERATION_ENABLED;
   if ((sw & 0x6F) == 0x07) return AX_QUICK_STOP_ACTIVE;
   if ((sw & 0x4F) == 0x0F) return AX_FAULT_REACTION_ACTIVE;
   if ((sw & 0x4F) == 0x08) return AX_FAULT;
   return AX_UNKNOWN;
}

const char *cia402_state_name(enum ipc_axis_state s)
{
   switch (s)
   {
   case AX_NOT_READY:             return "Not ready";
   case AX_SWITCH_ON_DISABLED:    return "Switch on disabled";
   case AX_READY_TO_SWITCH_ON:    return "Ready to switch on";
   case AX_SWITCHED_ON:           return "Switched on";
   case AX_OPERATION_ENABLED:     return "Enabled";
   case AX_QUICK_STOP_ACTIVE:     return "Quick stop";
   case AX_FAULT_REACTION_ACTIVE: return "Fault reaction";
   case AX_FAULT:                 return "Fault";
   default:                       return "Unknown";
   }
}

void cia402_request_enable(cia402_t *c)
{
   c->enable_req = true;
   c->enable_timeout = false;
   c->enable_cycles = 0;
}

void cia402_request_disable(cia402_t *c)
{
   c->enable_req = false;
}

void cia402_request_fault_reset(cia402_t *c, uint32_t pulse_cycles)
{
   c->enable_req = false;
   c->reset_cycles = pulse_cycles;
}

void cia402_force_disable(cia402_t *c)
{
   c->enable_req = false;
   c->enable_cycles = 0;
}

uint16_t cia402_step(cia402_t *c, uint16_t sw, bool ready_to_disable, uint32_t enable_timeout_cycles)
{
   enum ipc_axis_state s = cia402_decode(sw);

   if (c->reset_cycles > 0)
   {
      /* Xung 0 -> 0x80 (sườn lên) trong vài chu kỳ */
      c->reset_cycles--;
      return c->reset_cycles > 0 ? CW_FAULT_RESET : CW_DISABLE_VOLTAGE;
   }

   if (s == AX_FAULT || s == AX_FAULT_REACTION_ACTIVE)
   {
      c->enable_req = false;
      return CW_DISABLE_VOLTAGE;
   }

   if (!c->enable_req)
   {
      /* Disable khi đang chạy: giữ enable đến khi profile dừng hẳn rồi mới cắt. */
      if (s == AX_OPERATION_ENABLED && !ready_to_disable)
         return CW_ENABLE_OPERATION;
      return CW_DISABLE_VOLTAGE;
   }

   if (s == AX_OPERATION_ENABLED)
   {
      c->enable_cycles = 0;
      return CW_ENABLE_OPERATION;
   }

   if (++c->enable_cycles > enable_timeout_cycles)
   {
      c->enable_req = false;
      c->enable_timeout = true;
      return CW_DISABLE_VOLTAGE;
   }

   switch (s)
   {
   case AX_SWITCH_ON_DISABLED: return CW_SHUTDOWN;
   case AX_READY_TO_SWITCH_ON: return CW_SWITCH_ON;
   case AX_SWITCHED_ON:        return CW_ENABLE_OPERATION;
   default:                    return CW_DISABLE_VOLTAGE;
   }
}
