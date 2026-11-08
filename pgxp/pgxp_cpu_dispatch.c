/* Route recompiler tracking through the shared CPU instruction handlers.
 * Keeping the switch separate limits duplicated handler code from inlining. */
#include "pgxp_cpu.h"

#define op(_instr)		(_instr >> 26)			/* The op part of the instruction register */
#define func(_instr)	((_instr) & 0x3F)		/* The funct part of the instruction register */

/* Does this instruction have a PGXP CPU-mode tracker?  Pure opcode predicate. */
int PGXP_CPU_Tracks(uint32_t instr)
{
   switch (op(instr))
   {
      case 0x00: /* SPECIAL */
         switch (func(instr))
         {
            case 0x00: /* SLL  */ case 0x02: /* SRL  */ case 0x03: /* SRA  */
            case 0x04: /* SLLV */ case 0x06: /* SRLV */ case 0x07: /* SRAV */
            case 0x10: /* MFHI */ case 0x11: /* MTHI */
            case 0x12: /* MFLO */ case 0x13: /* MTLO */
            case 0x18: /* MULT */ case 0x19: /* MULTU */
            case 0x1A: /* DIV  */ case 0x1B: /* DIVU */
            case 0x20: /* ADD  */ case 0x21: /* ADDU */
            case 0x22: /* SUB  */ case 0x23: /* SUBU */
            case 0x24: /* AND  */ case 0x25: /* OR   */
            case 0x26: /* XOR  */ case 0x27: /* NOR  */
            case 0x2A: /* SLT  */ case 0x2B: /* SLTU */
               return 1;
            default:
               return 0;
         }
      case 0x08: /* ADDI  */ case 0x09: /* ADDIU */
      case 0x0A: /* SLTI  */ case 0x0B: /* SLTIU */
      case 0x0C: /* ANDI  */ case 0x0D: /* ORI   */
      case 0x0E: /* XORI  */ case 0x0F: /* LUI   */
      case 0x20: /* LB    */ case 0x21: /* LH    */
      case 0x22: /* LWL   */ case 0x23: /* LW    */
      case 0x24: /* LBU   */ case 0x25: /* LHU   */
      case 0x26: /* LWR   */
      case 0x28: /* SB    */ case 0x29: /* SH    */
      case 0x2A: /* SWL   */ case 0x2B: /* SW    */
      case 0x2E: /* SWR   */
         return 1;
      default:
         return 0;
   }
}

/* Route one tracked instruction to its PGXP_CPU_* tracker.
 *   rdVal : result written to Rd (R-type) or Rt (I-type / loads)
 *   rsVal : Rs source value      (also Rt source for stores)
 *   rtVal : Rt source value
 *   hiVal,loVal : HI/LO results (mult/div) or HI/LO sources (mfhi/mflo/...)
 *   addr  : effective memory address (loads/stores)
 * The recompiler passes the values it has; unused ones are ignored. */
void PGXP_CPU_Dispatch(uint32_t instr,
                       uint32_t rdVal, uint32_t rsVal, uint32_t rtVal,
                       uint32_t hiVal, uint32_t loVal, uint32_t addr)
{
   switch (op(instr))
   {
      case 0x00: /* SPECIAL */
         switch (func(instr))
         {
            case 0x00: PGXP_CPU_SLL  (instr, rdVal, rtVal);               break;
            case 0x02: PGXP_CPU_SRL  (instr, rdVal, rtVal);               break;
            case 0x03: PGXP_CPU_SRA  (instr, rdVal, rtVal);               break;
            case 0x04: PGXP_CPU_SLLV (instr, rdVal, rtVal, rsVal);        break;
            case 0x06: PGXP_CPU_SRLV (instr, rdVal, rtVal, rsVal);        break;
            case 0x07: PGXP_CPU_SRAV (instr, rdVal, rtVal, rsVal);        break;
            case 0x10: PGXP_CPU_MFHI (instr, rdVal, hiVal);               break;
            case 0x11: PGXP_CPU_MTHI (instr, hiVal, rsVal);               break;
            case 0x12: PGXP_CPU_MFLO (instr, rdVal, loVal);               break;
            case 0x13: PGXP_CPU_MTLO (instr, loVal, rsVal);               break;
            case 0x18: PGXP_CPU_MULT (instr, hiVal, loVal, rsVal, rtVal); break;
            case 0x19: PGXP_CPU_MULTU(instr, hiVal, loVal, rsVal, rtVal); break;
            case 0x1A: PGXP_CPU_DIV  (instr, hiVal, loVal, rsVal, rtVal); break;
            case 0x1B: PGXP_CPU_DIVU (instr, hiVal, loVal, rsVal, rtVal); break;
            case 0x20: PGXP_CPU_ADD  (instr, rdVal, rsVal, rtVal);        break;
            case 0x21: PGXP_CPU_ADDU (instr, rdVal, rsVal, rtVal);        break;
            case 0x22: PGXP_CPU_SUB  (instr, rdVal, rsVal, rtVal);        break;
            case 0x23: PGXP_CPU_SUBU (instr, rdVal, rsVal, rtVal);        break;
            case 0x24: PGXP_CPU_AND  (instr, rdVal, rsVal, rtVal);        break;
            case 0x25: PGXP_CPU_OR   (instr, rdVal, rsVal, rtVal);        break;
            case 0x26: PGXP_CPU_XOR  (instr, rdVal, rsVal, rtVal);        break;
            case 0x27: PGXP_CPU_NOR  (instr, rdVal, rsVal, rtVal);        break;
            case 0x2A: PGXP_CPU_SLT  (instr, rdVal, rsVal, rtVal);        break;
            case 0x2B: PGXP_CPU_SLTU (instr, rdVal, rsVal, rtVal);        break;
            default: break;
         }
         break;
      /* I-type: rdVal carries the Rt result, rsVal the Rs source. */
      case 0x08: PGXP_CPU_ADDI (instr, rdVal, rsVal); break;
      case 0x09: PGXP_CPU_ADDIU(instr, rdVal, rsVal); break;
      case 0x0A: PGXP_CPU_SLTI (instr, rdVal, rsVal); break;
      case 0x0B: PGXP_CPU_SLTIU(instr, rdVal, rsVal); break;
      case 0x0C: PGXP_CPU_ANDI (instr, rdVal, rsVal); break;
      case 0x0D: PGXP_CPU_ORI  (instr, rdVal, rsVal); break;
      case 0x0E: PGXP_CPU_XORI (instr, rdVal, rsVal); break;
      case 0x0F: PGXP_CPU_LUI  (instr, rdVal);        break;
      /* Loads: rdVal carries the Rt result (narrowed for sub-word). */
      case 0x20: PGXP_CPU_LB (instr, (uint8_t) rdVal, addr); break;
      case 0x21: PGXP_CPU_LH (instr, (uint16_t)rdVal, addr); break;
      case 0x22: PGXP_CPU_LWL(instr, rdVal, addr);           break;
      case 0x23: PGXP_CPU_LW (instr, rdVal, addr);           break;
      case 0x24: PGXP_CPU_LBU(instr, (uint8_t) rdVal, addr); break;
      case 0x25: PGXP_CPU_LHU(instr, (uint16_t)rdVal, addr); break;
      case 0x26: PGXP_CPU_LWR(instr, rdVal, addr);           break;
      /* Stores: rtVal carries the Rt source being written out. */
      case 0x28: PGXP_CPU_SB (instr, (uint8_t) rtVal, addr); break;
      case 0x29: PGXP_CPU_SH (instr, (uint16_t)rtVal, addr); break;
      case 0x2A: PGXP_CPU_SWL(instr, rtVal, addr);           break;
      case 0x2B: PGXP_CPU_SW (instr, rtVal, addr);           break;
      case 0x2E: PGXP_CPU_SWR(instr, rtVal, addr);           break;
      default: break;
   }
}
