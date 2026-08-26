/* PGXP precise-colour transport check.
 *
 * The oracle check (oracle.c) proves that an accepted shadow is correct.
 * It injects the shadow straight into the command buffer with
 * PGXP_WriteCB, so it says nothing about whether a colour actually
 * *survives* the journey from the GTE to a GP0 packet. That journey is
 * this file's subject, and it is the half that decides whether the
 * hit-rate measurement can be believed: a near-zero hit rate in content
 * means "games recolour" only if transport is known to work.
 *
 * Everything here drives the real PGXP functions - GTE register hooks,
 * CPU register/memory tracking, the GPU FIFO -> command-buffer copy - in
 * the same order gte.c / pgxp_cpu.c / gpu.c call them. Nothing is
 * mirrored except the two MIPS instruction words, which are assembled by
 * hand.
 *
 * Build and run: make -C tools/pgxp_color check
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../../pgxp/pgxp_gpu.h"
#include "../../pgxp/pgxp_gte.h"
#include "../../pgxp/pgxp_cpu.h"
#include "../../pgxp/pgxp_diag.h"
#include "../../pgxp/pgxp_lineage.h"
#include "../../pgxp/pgxp_mem.h"
#include "../../pgxp/pgxp_main.h"
#include "../../pgxp/pgxp_types.h"
#include "../../pgxp/pgxp_value.h"

/* Replica of gte.c's ColorFIFO pack; see oracle.c for why it is mirrored
 * rather than included. */
static uint8_t lm_c_replica(int32_t v)
{
   if (v < 0)
      return 0;
   if (v > 0xFF)
      return 0xFF;
   return (uint8_t)v;
}

static uint32_t gte_pack(int32_t m1, int32_t m2, int32_t m3, uint8_t cd)
{
   return  (uint32_t)lm_c_replica(m1 >> 4)
        | ((uint32_t)lm_c_replica(m2 >> 4) << 8)
        | ((uint32_t)lm_c_replica(m3 >> 4) << 16)
        | ((uint32_t)cd << 24);
}

/* MIPS instruction assembly, only the fields the PGXP hooks decode. */
#define INSTR_RT(rt)          (((uint32_t)(rt) & 0x1F) << 16)
#define INSTR_RD(rd)          (((uint32_t)(rd) & 0x1F) << 11)
#define INSTR_RS(rs)          (((uint32_t)(rs) & 0x1F) << 21)
#define INSTR_SA(sa)          (((uint32_t)(sa) & 0x1F) << 6)
#define INSTR_OP(op)          (((uint32_t)(op) & 0x3F) << 26)

/* Scratch addresses in tracked RAM. */
#define LIST_ADDR   0x80100000u
#define LIST_ADDR2  0x80100004u

static int failures;

static void fail(const char* what, uint32_t got, uint32_t want)
{
   fprintf(stderr, "FAIL %-34s got=%08x want=%08x\n", what, got, want);
   failures++;
}

/* Push a colour through the GTE side exactly as MAC_to_RGB_FIFO does. */
static uint32_t gte_produce(int32_t m1, int32_t m2, int32_t m3, uint8_t cd)
{
   uint32_t packed = gte_pack(m1, m2, m3, cd);
   PGXP_pushRGBf((float)m1 / 16.0f, (float)m2 / 16.0f,
                 (float)m3 / 16.0f, packed);
   return packed;
}

/* The GPU side: gpu.c writes each command word into the blitter FIFO
 * paired with the tracked value read from the address it came from, then
 * ProcessFIFO copies the FIFO slots into the command buffer. */
static void gpu_deliver(const uint32_t* addrs, const uint32_t* words,
                        unsigned n)
{
   unsigned i;
   for (i = 0; i < n; i++)
   {
      PGXP_LineageFIFOWrite(i, addrs[i], words[i]);
      PGXP_DiagFIFOWrite(i, addrs[i], words[i], ReadMem(addrs[i]));
      PGXP_WriteFIFO(ReadMem(addrs[i]), i);   /* GPU_WriteCB */
   }
   for (i = 0; i < n; i++)
   {
      PGXP_LineageCBWrite(i, i);
      PGXP_DiagCBWrite(i, i);
      PGXP_WriteCB(PGXP_ReadFIFO(i), i);      /* ProcessFIFO */
   }
}

/* ---------------------------------------------------------------------
 * Idiom A: swc2 $22 -> display list. The GTE's CD byte already holds the
 * GP0 command code, so the whole word goes straight out.
 */
static void test_swc2_direct(void)
{
   int32_t  m1 = 0x0A37, m2 = 0x1B04, m3 = 0x0055;
   uint32_t packed, addr = LIST_ADDR;
   uint32_t instr = INSTR_RT(22);
   float    rgb[3];

   packed = gte_produce(m1, m2, m3, 0x30);   /* 0x30 = shaded triangle */

   /* swc2 $22, 0(rs) : Mem[addr] = GTE_D[22] */
   PGXP_GTE_SWC2(instr, packed, addr);

   gpu_deliver(&addr, &packed, 1);

   if (!PGXP_GetColor(0, &packed, rgb))
   {
      fail("swc2 direct: not accepted", 0, 1);
      return;
   }
   if (rgb[0] != (float)m1 / 16.0f ||
       rgb[1] != (float)m2 / 16.0f ||
       rgb[2] != (float)m3 / 16.0f)
      fail("swc2 direct: wrong payload", 0, 0);
}

/* ---------------------------------------------------------------------
 * Idiom B: mfc2 into a GPR, ori the GP0 opcode into byte 3, sw to the
 * list. This is the path the accept rule's 24-bit compare exists for.
 */
static void test_mfc2_ori_sw(void)
{
   int32_t  m1 = 0x0100, m2 = 0x0F80, m3 = 0x0C21;
   uint32_t packed, with_cmd, addr = LIST_ADDR2;
   float    rgb[3];

   packed = gte_produce(m1, m2, m3, 0x00);

   /* mfc2 $t0, $22 : CPU[8] = GTE_D[22] */
   PGXP_GTE_MFC2(INSTR_RT(8) | INSTR_RD(22), packed, packed);

   /* ori $t0, $t0, ... - the opcode byte is above the 16-bit immediate,
    * so real code uses lui+or; model it as an OR of two GPRs, which is
    * the same tracking path (PGXP_CPU_OR). */
   with_cmd = (packed & 0x00FFFFFFu) | (0x38u << 24);
   PGXP_CPU_OR(INSTR_RD(8) | INSTR_RS(8) | INSTR_RT(9),
               with_cmd, packed, 0x38u << 24);

   /* sw $t0, 0(rs) */
   PGXP_CPU_SW(INSTR_OP(0x2B) | INSTR_RT(8), with_cmd, addr);

   gpu_deliver(&addr, &with_cmd, 1);

   if (!PGXP_GetColor(0, &with_cmd, rgb))
   {
      /* Not a hard failure: PGXP's bitwise-op tracking may legitimately
       * invalidate here (see pgxp_cpu.c INVALID_BITWISE_OP). Report it,
       * because it bounds what the hit rate can be in content that uses
       * this idiom. */
      printf("    note: mfc2+or+sw not tracked through the bitwise op "
             "(expected if PGXP invalidates on OR)\n");
      return;
   }
   if (rgb[0] != (float)m1 / 16.0f ||
       rgb[1] != (float)m2 / 16.0f ||
       rgb[2] != (float)m3 / 16.0f)
      fail("mfc2+or+sw: wrong payload", 0, 0);
   else
      printf("    note: mfc2+or+sw survives transport\n");
}

/* ---------------------------------------------------------------------
 * A multi-word packet: three gouraud vertices, colour words interleaved
 * with vertex words, checked at their real command-buffer offsets.
 */
static void test_gouraud_packet(void)
{
   /* GP0(0x30): colour0, xy0, colour1, xy1, colour2, xy2 */
   uint32_t addrs[6], words[6];
   int32_t  macs[3][3] = { { 0x0333, 0x0444, 0x0555 },
                           { 0x0666, 0x0777, 0x0888 },
                           { 0x0999, 0x0AAA, 0x0BBB } };
   unsigned v;
   float    rgb[3];

   for (v = 0; v < 3; v++)
   {
      uint32_t caddr = 0x80101000u + v * 8u;
      uint32_t vaddr = caddr + 4u;
      uint32_t packed = gte_produce(macs[v][0], macs[v][1], macs[v][2],
                                    v == 0 ? 0x30 : 0x00);
      PGXP_GTE_SWC2(INSTR_RT(22), packed, caddr);

      addrs[v * 2]     = caddr;
      words[v * 2]     = packed;
      addrs[v * 2 + 1] = vaddr;
      words[v * 2 + 1] = 0x00400040u;   /* an untracked vertex word */
   }

   gpu_deliver(addrs, words, 6);

   for (v = 0; v < 3; v++)
   {
      if (!PGXP_GetColor(v * 2, &words[v * 2], rgb))
      {
         fail("gouraud packet: colour not accepted", v, 1);
         continue;
      }
      if (rgb[0] != (float)macs[v][0] / 16.0f ||
          rgb[1] != (float)macs[v][1] / 16.0f ||
          rgb[2] != (float)macs[v][2] / 16.0f)
         fail("gouraud packet: wrong payload", v, 0);
   }

   /* The vertex slots must not be mistaken for colours. */
   if (PGXP_GetColor(1, &words[1], rgb))
      fail("gouraud packet: vertex word accepted as colour", 1, 0);
}

/* ---------------------------------------------------------------------
 * FIFO ordering: three colours pushed through the GTE in sequence must
 * arrive at their own slots, not shuffled. Catches an off-by-one in the
 * DR[20..22] shadow shift.
 */
static void test_fifo_order(void)
{
   uint32_t addrs[3], words[3];
   unsigned i;
   float    rgb[3];
   int32_t  base[3] = { 0x0110, 0x0220, 0x0330 };

   for (i = 0; i < 3; i++)
   {
      addrs[i] = 0x80102000u + i * 4u;
      words[i] = gte_produce(base[i], base[i] + 1, base[i] + 2, 0x20);
      PGXP_GTE_SWC2(INSTR_RT(22), words[i], addrs[i]);
   }

   gpu_deliver(addrs, words, 3);

   for (i = 0; i < 3; i++)
   {
      if (!PGXP_GetColor(i, &words[i], rgb))
      {
         fail("fifo order: not accepted", i, 1);
         continue;
      }
      if (rgb[0] != (float)base[i] / 16.0f)
         fail("fifo order: colours shuffled", i, 0);
   }
}

/* ---------------------------------------------------------------------
 * Negative control: a colour word the game composed on the CPU without
 * the GTE must NOT be accepted with a stale shadow attached.
 */
static void test_untracked_colour(void)
{
   uint32_t addr = 0x80103000u;
   uint32_t stale, cpu_word;
   float    rgb[3];

   /* Produce something through the GTE and land it at `addr`... */
   stale = gte_produce(0x0500, 0x0500, 0x0500, 0x30);
   PGXP_GTE_SWC2(INSTR_RT(22), stale, addr);

   /* ...then have the CPU overwrite that list slot with a different
    * colour. PGXP_CPU_SW with an untracked register must clear the
    * tracking, so the stale shadow cannot be reused. */
   cpu_word = 0x30204060u;
   PGXP_CPU_SW(INSTR_OP(0x2B) | INSTR_RT(15), cpu_word, addr);

   gpu_deliver(&addr, &cpu_word, 1);

   if (PGXP_GetColor(0, &cpu_word, rgb))
      fail("untracked colour accepted from stale shadow", 0, 0);
}

static void set_cpu_value(unsigned reg, uint32_t value)
{
   CPU_reg[reg] = PGXP_value_zero;
   SetValue(&CPU_reg[reg], value);
}


static void test_exact_lineage_transport(void)
{
   const float precise_x = 123.25f;
   const float precise_y = -45.5f;
   const uint32_t packed = 0xFFD3007Bu;
   const uint32_t sll = packed << 5;
   const uint32_t sra = (uint32_t)((int32_t)sll >> 5);
   const uint32_t scratch_addr = 0x1F800000u;
   const uint32_t final_addr = 0x80106004u;
   uint32_t instr;
   OGLVertex output;
   float recovered_x, recovered_y, recovered_z;
   int recovered_w;

   PGXP_SetExperimentMask(PGXP_FEATURE_ALL);
   PGXP_LineageReset();
   PGXP_pushSXYZ2f(precise_x, precise_y, 32768.0f, packed);
   PGXP_GTE_MFC2(INSTR_RT(8) | INSTR_RD(14), packed, packed);

   /* I owns the direct MFC2 alias and preserves the ordinary CPU shadow. */
   memset(&CPU_reg[7], 0, sizeof(CPU_reg[7]));
   instr = INSTR_RS(8) | INSTR_RT(0) | INSTR_RD(7) | 0x21u;
   PGXP_CPU_LineageIdentityMove(instr, packed, packed);
   if ((CPU_reg[7].flags & VALID_01) != VALID_01 ||
       CPU_reg[7].x != precise_x || CPU_reg[7].y != precise_y)
      fail("identity move lost MFC2 precision", 0, 1);

   /* J proves both shifts but never installs their payload in CPU_reg. */
   memset(&CPU_reg[9], 0, sizeof(CPU_reg[9]));
   instr = INSTR_RT(7) | INSTR_RD(9) | INSTR_SA(5);
   PGXP_CPU_LineageShift(instr, packed, sll, 0);
   if ((CPU_reg[9].flags & VALID_01) == VALID_01)
      fail("SLL5 lineage mutated CPU shadow", 1, 0);

   memset(&CPU_reg[11], 0, sizeof(CPU_reg[11]));
   instr = INSTR_RS(9) | INSTR_RT(0) | INSTR_RD(11) | 0x21u;
   PGXP_CPU_LineageIdentityMove(instr, sll, sll);
   if ((CPU_reg[11].flags & VALID_01) == VALID_01)
      fail("stage-2 alias mutated CPU shadow", 1, 0);
   PGXP_CPU_SW(INSTR_OP(0x2B) | INSTR_RT(11), sll, scratch_addr);

   memset(&CPU_reg[10], 0, sizeof(CPU_reg[10]));
   PGXP_CPU_LW((0x23u << 26) | INSTR_RT(10), sll, scratch_addr);
   instr = INSTR_RT(10) | INSTR_RD(10) | INSTR_SA(5) | 0x03u;
   PGXP_CPU_LineageShift(instr, sll, sra, 1);
   if ((CPU_reg[10].flags & VALID_01) == VALID_01)
      fail("SRA5 lineage mutated CPU shadow", 1, 0);

   memset(&CPU_reg[12], 0, sizeof(CPU_reg[12]));
   instr = INSTR_RS(10) | INSTR_RT(0) | INSTR_RD(12) | 0x21u;
   PGXP_CPU_LineageIdentityMove(instr, sra, sra);
   PGXP_CPU_SW(INSTR_OP(0x2B) | INSTR_RT(12), sra, final_addr);
   gpu_deliver(&final_addr, &sra, 1);

   if (!PGXP_LineageRecoverVertex(0, sra, &recovered_x, &recovered_y,
       &recovered_z, &recovered_w) || recovered_x != precise_x ||
       recovered_y != precise_y || recovered_z != 1.0f || !recovered_w)
      fail("exact sidecar recovery missed", 0, 1);
   if (PGXP_LineageRecoverVertex(0, sra ^ 1u, &recovered_x, &recovered_y,
       &recovered_z, &recovered_w))
      fail("exact sidecar accepted wrong word", 1, 0);

   memset(&output, 0, sizeof(output));
   PGXP_GetVertex(0, &sra, &output, 0, 0);
   if (output.x != precise_x || output.y != precise_y ||
       !output.valid_w || output.w != (1.0f / 32768.0f))
      fail("production lineage GPU handoff missed", 0, 1);

   /* Partial stores share Beetle's ordinary PGXP store helper, but are not
    * exact word aliases and therefore must only retire the sidecar. */
   PGXP_CPU_SWL(INSTR_OP(0x2A) | INSTR_RT(12), sra, final_addr);
   gpu_deliver(&final_addr, &sra, 1);
   if (PGXP_LineageRecoverVertex(0, sra, &recovered_x, &recovered_y,
       &recovered_z, &recovered_w))
      fail("sidecar survived partial store", 1, 0);

   /* A subsequent proven full-word store may establish it again. */
   PGXP_CPU_SW(INSTR_OP(0x2B) | INSTR_RT(12), sra, final_addr);
   gpu_deliver(&final_addr, &sra, 1);
   if (!PGXP_LineageRecoverVertex(0, sra, &recovered_x, &recovered_y,
       &recovered_z, &recovered_w))
      fail("sidecar did not survive full SW", 0, 1);

   /* A same-value architectural overwrite without lineage must retire it. */
   PGXP_GTE_SWC2(INSTR_RT(14), sra, final_addr);
   gpu_deliver(&final_addr, &sra, 1);
   if (PGXP_LineageRecoverVertex(0, sra, &recovered_x, &recovered_y,
       &recovered_z, &recovered_w))
      fail("sidecar survived unproven overwrite", 1, 0);

   PGXP_SetExperimentMask(PGXP_FEATURE_ALL & ~PGXP_FEATURE_EXACT_LINEAGE);
   PGXP_LineageReset();
   PGXP_pushSXYZ2f(precise_x, precise_y, 32768.0f, packed);
   PGXP_GTE_MFC2(INSTR_RT(8) | INSTR_RD(14), packed, packed);
   instr = INSTR_RT(8) | INSTR_RD(9) | INSTR_SA(5);
   PGXP_CPU_LineageShift(instr, packed, sll, 0);
   if (PGXP_LineageRecoverVertex(0, sra, &recovered_x, &recovered_y,
       &recovered_z, &recovered_w))
      fail("exact-lineage feature gate inactive", 1, 0);

   PGXP_SetExperimentMask(PGXP_FEATURE_ALL & ~PGXP_FEATURE_IDENTITY_MOVE);
   PGXP_LineageReset();
   PGXP_pushSXYZ2f(precise_x, precise_y, 32768.0f, packed);
   PGXP_GTE_MFC2(INSTR_RT(8) | INSTR_RD(14), packed, packed);
   memset(&CPU_reg[7], 0, sizeof(CPU_reg[7]));
   instr = INSTR_RS(8) | INSTR_RT(0) | INSTR_RD(7) | 0x21u;
   PGXP_CPU_LineageIdentityMove(instr, packed, packed);
   if ((CPU_reg[7].flags & VALID_01) == VALID_01)
      fail("identity-move feature gate inactive", 1, 0);

   PGXP_SetExperimentMask(PGXP_FEATURE_ALL);
   PGXP_LineageReset();
}

static void test_cpu_math_invariants(void)
{
   uint32_t instr;

   set_cpu_value(1, 0x12345678u);
   set_cpu_value(2, 0x00FF0F0Fu);
   CPU_reg[1].gFlags = 0xFFFFu;
   CPU_reg[1].count = 0xDEADBEEFu;
   instr = INSTR_RS(1) | INSTR_RT(2) | INSTR_RD(3) | 0x24u;
   PGXP_CPU_AND(instr, 0x00340608u, 0x12345678u, 0x00FF0F0Fu);
   if (CPU_reg[3].count != 0 || CPU_reg[3].gFlags != 0)
      fail("AND result metadata not initialized", CPU_reg[3].count, 0);

   instr = INSTR_RS(1) | INSTR_RT(2) | INSTR_RD(3) | 0x2Au;
   set_cpu_value(1, 0x00010000u);
   set_cpu_value(2, 0x0000FFFFu);
   PGXP_CPU_SLT(instr, 0, 0x00010000u, 0x0000FFFFu);
   if (CPU_reg[3].x != 0.f)
      fail("SLT compared low halves with high unequal", 1, 0);

   instr = INSTR_RS(1) | INSTR_RT(2) | INSTR_RD(3) | 0x2Bu;
   PGXP_CPU_SLTU(instr, 0, 0x00010000u, 0x0000FFFFu);
   if (CPU_reg[3].x != 0.f)
      fail("SLTU compared low halves with high unequal", 1, 0);
}

#if PGXP_DIAG
static void test_vertex_w_provenance_gate(void)
{
   const uint32_t packed = 0xFFE4007Bu;
   PGXP_value shadow = PGXP_value_zero;
   OGLVertex out;

   shadow.x = 123.25f;
   shadow.y = -27.5f;
   shadow.z = 4096.0f;
   shadow.value = packed;
   shadow.flags = VALID_012;
   PGXP_WriteCB(&shadow, 0);
   memset(&out, 0, sizeof(out));
   PGXP_GetVertex(0, &packed, &out, 0, 0);
   if (out.x != shadow.x || out.y != shadow.y)
      fail("W gate discarded precise XY", 0, 1);
   if (out.valid_w)
      fail("unproven stage-0 W accepted", 1, 0);

   PGXP_DiagTraceGTE(&shadow);
   PGXP_WriteCB(&shadow, 0);
   memset(&out, 0, sizeof(out));
   PGXP_GetVertex(0, &packed, &out, 0, 0);
	if (!out.valid_w || out.w != shadow.z / 32768.0f)
		fail("GTE-proven W rejected", out.valid_w, 1);
}
#endif

static void test_nclip_sign_only(void)
{
   if (PGXP_NCLIP_sign_only(1234, -77) != -1234)
      fail("NCLIP positive sign replacement", PGXP_NCLIP_sign_only(1234, -77), -1234);
   if (PGXP_NCLIP_sign_only(-4321, 88) != 4321)
      fail("NCLIP negative sign replacement", PGXP_NCLIP_sign_only(-4321, 88), 4321);
   if (PGXP_NCLIP_sign_only(1234, 77) != 1234)
      fail("NCLIP same sign changed magnitude", PGXP_NCLIP_sign_only(1234, 77), 1234);
   if (PGXP_NCLIP_sign_only(-4321, -88) != -4321)
      fail("NCLIP same negative changed magnitude", PGXP_NCLIP_sign_only(-4321, -88), -4321);
   if (PGXP_NCLIP_sign_only(1234, 0) != 1234)
      fail("NCLIP precise zero replaced native", PGXP_NCLIP_sign_only(1234, 0), 1234);
   if (PGXP_NCLIP_sign_only(0, -88) != -1)
      fail("NCLIP native zero lost precise sign", PGXP_NCLIP_sign_only(0, -88), -1);
}


int main(void)
{
   PGXP_Init();
   PGXP_InitMem();
   PGXP_InitGTE();
   PGXP_SetModes(PGXP_MODE_MEMORY);

   printf("[T1] swc2 $22 -> display list -> GP0\n");
   test_swc2_direct();

   printf("[T2] mfc2 + or(cmd byte) + sw -> GP0\n");
   test_mfc2_ori_sw();

   printf("[T3] three-vertex gouraud packet, colours at real offsets\n");
   test_gouraud_packet();

   printf("[T4] ColorFIFO ordering across three pushes\n");
   test_fifo_order();

   printf("[T5] negative control: CPU-composed colour must be refused\n");
   test_untracked_colour();

   printf("[T6] exact MFC2/shift sidecar and identity transport\n");
   test_exact_lineage_transport();

   printf("[T7] CPU bitwise/comparison invariants\n");
   test_cpu_math_invariants();

   printf("[T8] vertex W provenance gate preserves precise XY\n");
#if PGXP_DIAG
   test_vertex_w_provenance_gate();
#else
   printf("    skipped: W provenance instrumentation is diagnostic-only\n");
#endif

   printf("[T9] NCLIP precise sign preserves native magnitude\n");
   test_nclip_sign_only();

   if (failures)
   {
      printf("\nfailures=%d\nFAIL\n", failures);
      return 1;
   }
   printf("\nFAIL count 0\nPASS\n");
   return 0;
}
