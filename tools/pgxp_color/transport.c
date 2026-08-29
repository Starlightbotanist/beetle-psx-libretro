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
#define INSTR_OP(op)          (((uint32_t)(op) & 0x3F) << 26)
#define INSTR_SA(sa)          (((uint32_t)(sa) & 0x1F) << 6)

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
      PGXP_WriteFIFO(ReadMem(addrs[i]), i);   /* GPU_WriteCB */
   for (i = 0; i < n; i++)
      PGXP_WriteCB(PGXP_ReadFIFO(i), i);      /* ProcessFIFO */
   (void)words;
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
   if (GTE_data_reg[22].flags & VALID_PROJECTION)
      fail("colour marked as projected vertex", 1, 0);

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
   PGXP_CPU_SW(INSTR_RT(8), with_cmd, addr);

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
   PGXP_CPU_SW(INSTR_RT(15), cpu_word, addr);   /* $15 never tracked */

   gpu_deliver(&addr, &cpu_word, 1);

   if (PGXP_GetColor(0, &cpu_word, rgb))
      fail("untracked colour accepted from stale shadow", 0, 0);
}

static void set_cpu_value(unsigned reg, uint32_t value)
{
   CPU_reg[reg] = PGXP_value_zero;
   SetValue(&CPU_reg[reg], value);
}

static void test_cpu_result_initialization(void)
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
}

static void test_cpu_comparison_ordering(void)
{
   uint32_t instr;

   set_cpu_value(1, 0x00010000u);
   set_cpu_value(2, 0x0000FFFFu);

   instr = INSTR_RS(1) | INSTR_RT(2) | INSTR_RD(3) | 0x2Au;
   PGXP_CPU_SLT(instr, 0, 0x00010000u, 0x0000FFFFu);
   if (CPU_reg[3].x != 0.f)
      fail("SLT used low half with unequal high halves", 1, 0);

   instr = INSTR_RS(1) | INSTR_RT(2) | INSTR_RD(3) | 0x2Bu;
   PGXP_CPU_SLTU(instr, 0, 0x00010000u, 0x0000FFFFu);
   if (CPU_reg[3].x != 0.f)
      fail("SLTU used low half with unequal high halves", 1, 0);

   set_cpu_value(1, 0x00000001u);
   set_cpu_value(2, 0x00000002u);
   instr = INSTR_RS(1) | INSTR_RT(2) | INSTR_RD(3) | 0x2Au;
   PGXP_CPU_SLT(instr, 1, 0x00000001u, 0x00000002u);
   if (CPU_reg[3].x != 1.f)
      fail("SLT did not compare equal-high low halves", 0, 1);
}

static void test_native_zero_add_identity(void)
{
   const uint32_t add_instr = INSTR_RS(4) | INSTR_RT(3) |
      INSTR_RD(5) | 0x21u;
   const uint32_t sub_instr = INSTR_RS(4) | INSTR_RT(3) |
      INSTR_RD(5) | 0x23u;

   set_cpu_value(4, 0x00000001u);
   set_cpu_value(3, 0x00000000u);
   CPU_reg[4].x = 1.25f;
   CPU_reg[3].x = 0.5f;
   PGXP_CPU_ADDU(add_instr, 0x00000001u, 0x00000001u,
      0x00000000u);
   if (CPU_reg[5].x != 1.25f)
      fail("right-zero ADD lost identity", 0, 1);

   set_cpu_value(4, 0x00000000u);
   set_cpu_value(3, 0x00000001u);
   CPU_reg[4].x = 0.5f;
   CPU_reg[3].x = 1.25f;
   PGXP_CPU_ADDU(add_instr, 0x00000001u, 0x00000000u,
      0x00000001u);
   if (CPU_reg[5].x != 1.75f)
      fail("left-zero ADD changed behavior", 0, 1);

   set_cpu_value(4, 0x00000001u);
   set_cpu_value(3, 0x00000000u);
   CPU_reg[4].x = 1.25f;
   CPU_reg[3].x = 0.5f;
   PGXP_CPU_SUBU(sub_instr, 0x00000001u, 0x00000001u,
      0x00000000u);
   if (CPU_reg[5].x != 0.75f)
      fail("right-zero SUB changed behavior", 0, 1);
}

static uint32_t pack_vertex(int16_t x, int16_t y)
{
   return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void test_projection_provenance(void)
{
   PGXP_value tracked = PGXP_value_zero;
   OGLVertex vertex;
   uint32_t word;

   word = pack_vertex(12, 34);
   PGXP_pushSXYZ2f(12.f, 34.f, 2.f, word);
   if (!(GTE_data_reg[14].flags & VALID_PROJECTION))
      fail("projected vertex missing provenance", 0, 1);

   word = pack_vertex(100, -100);
   tracked.x = 100.5f;
   tracked.y = -100.25f;
   tracked.z = 2.f;
   tracked.value = word;
   tracked.flags = VALID_012 | VALID_PROJECTION;
   PGXP_WriteCB(&tracked, 0);

   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   if (!vertex.valid_w || vertex.w != 2.f)
      fail("projected vertex W was rejected", vertex.valid_w, 1);

   tracked.flags = VALID_012;
   PGXP_WriteCB(&tracked, 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   if (vertex.valid_w)
      fail("unproven vertex W was accepted", 1, 0);
   if (vertex.x != 100.5f || vertex.y != -100.25f)
      fail("W rejection discarded precise XY", 1, 0);
}

static void test_nclip_magnitude(void)
{
   if (PGXP_NCLIP_preserve_magnitude(1234, -77) != -1234)
      fail("NCLIP positive orientation replacement", 0, 1);
   if (PGXP_NCLIP_preserve_magnitude(-4321, 88) != 4321)
      fail("NCLIP negative orientation replacement", 0, 1);
   if (PGXP_NCLIP_preserve_magnitude(1234, 77) != 1234)
      fail("NCLIP changed matching magnitude", 0, 1);
   if (PGXP_NCLIP_preserve_magnitude(1234, 0) != 1234)
      fail("NCLIP replaced native result with zero", 0, 1);
   if (PGXP_NCLIP_preserve_magnitude(0, -88) != -1)
      fail("NCLIP lost orientation at native zero", 0, 1);
   if (PGXP_NCLIP_preserve_magnitude(INT32_MIN, 1) != INT32_MAX)
      fail("NCLIP overflowed reversed minimum", 0, 1);
}

static void test_projection_provenance_transport(void)
{
   const uint32_t packed = pack_vertex(123, -45);
   const uint32_t addr = 0x80105000u;
   uint32_t instr;

   PGXP_pushSXYZ2f(123.25f, -45.5f, 32768.f, packed);
   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);
   if (!(CPU_reg[8].flags & VALID_PROJECTION))
      fail("MFC2 lost projection provenance", 0, 1);

   instr = INSTR_RS(8) | INSTR_RT(8) | INSTR_OP(0x0d) | 1u;
   PGXP_CPU_ObserveInstruction(instr);
   if (CPU_reg[8].flags & VALID_PROJECTION)
      fail("untracked CPU write kept provenance", 1, 0);

   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);
   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(8), packed, addr);
   PGXP_CPU_LW(INSTR_OP(0x23) | INSTR_RT(9), packed, addr);
   PGXP_CPU_ObserveInstruction(INSTR_OP(0x23) | INSTR_RT(9));
   if (!(CPU_reg[9].flags & VALID_PROJECTION))
      fail("full-word transport lost provenance", 0, 1);

   PGXP_CPU_SWL(INSTR_OP(0x2a) | INSTR_RT(8), packed, addr);
   if (ReadMem(addr)->flags & VALID_PROJECTION)
      fail("partial store kept provenance", 1, 0);
}

static void test_projection_identity_moves(void)
{
   const uint32_t packed = pack_vertex(321, -87);
   uint32_t instr;

   PGXP_pushSXYZ2f(321.25f, -87.5f, 32768.f, packed);
   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);

   instr = INSTR_RS(8) | INSTR_RT(0) | INSTR_RD(9) | 0x21u;
   PGXP_CPU_ADDU(instr, packed, packed, 0);
   PGXP_CPU_ObserveInstruction(instr);
   if ((CPU_reg[9].flags & (VALID_01 | VALID_PROJECTION)) !=
       (VALID_01 | VALID_PROJECTION) ||
       CPU_reg[9].x != 321.25f || CPU_reg[9].y != -87.5f)
      fail("register identity lost projected value", 0, 1);

   set_cpu_value(12, 0);
   CPU_reg[12].x = 0.5f;
   instr = INSTR_RS(8) | INSTR_RT(12) | INSTR_RD(13) | 0x21u;
   PGXP_CPU_ADDU(instr, packed, packed, 0);
   PGXP_CPU_ObserveInstruction(instr);
   if ((CPU_reg[13].flags & (VALID_01 | VALID_PROJECTION)) !=
       (VALID_01 | VALID_PROJECTION) ||
       CPU_reg[13].x != 321.25f || CPU_reg[13].y != -87.5f)
      fail("native-zero identity lost projection", 0, 1);

   instr = INSTR_RS(9) | INSTR_RT(10) | INSTR_OP(0x0d);
   PGXP_CPU_ORI(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);
   if ((CPU_reg[10].flags & VALID_PROJECTION) != VALID_PROJECTION ||
       CPU_reg[10].x != 321.25f || CPU_reg[10].y != -87.5f)
      fail("immediate identity lost projected value", 0, 1);

   CPU_reg[10].value ^= 1u;
   instr = INSTR_RS(10) | INSTR_RT(11) | INSTR_OP(0x0e);
   PGXP_CPU_XORI(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);
   if (CPU_reg[11].flags & VALID_PROJECTION)
      fail("identity accepted mismatched source", 1, 0);
}

static void test_memory_mode_dispatch(void)
{
   const uint32_t packed = pack_vertex(211, -63);
   uint32_t instr;

   PGXP_pushSXYZ2f(211.25f, -63.5f, 32768.f, packed);
   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);

   instr = INSTR_RS(8) | INSTR_RT(0) | INSTR_RD(9) | 0x21u;
   PGXP_CPU_MemoryDispatch(instr, packed, packed, 0);
   PGXP_CPU_ObserveInstruction(instr);
   if ((CPU_reg[9].flags & (VALID_01 | VALID_PROJECTION)) !=
       (VALID_01 | VALID_PROJECTION) ||
      CPU_reg[9].x != 211.25f || CPU_reg[9].y != -63.5f)
      fail("memory dispatch lost identity projection", 0, 1);

   CPU_reg[10] = CPU_reg[9];
   instr = INSTR_RS(9) | INSTR_RT(10) | INSTR_OP(0x0c) | 0xffffu;
   PGXP_CPU_MemoryDispatch(instr, packed, packed, 0);
   PGXP_CPU_ObserveInstruction(instr);
   if (CPU_reg[10].flags & VALID_PROJECTION)
      fail("memory dispatch kept changed projection", 1, 0);
}

static void test_projection_write_observer(void)
{
   CPU_reg[31] = CPU_reg[9];
   PGXP_CPU_ObserveInstruction(INSTR_OP(0x01) | INSTR_RT(0x00));
   if (!(CPU_reg[31].flags & VALID_PROJECTION))
      fail("non-linking branch cleared link register", 0, 1);

   PGXP_CPU_ObserveInstruction(INSTR_OP(0x01) | INSTR_RT(0x11));
   if (CPU_reg[31].flags & VALID_PROJECTION)
      fail("linking branch kept stale projection", 1, 0);

   CPU_reg[31] = CPU_reg[9];
   PGXP_CPU_ObserveInstruction(INSTR_OP(0x03));
   if (CPU_reg[31].flags & VALID_PROJECTION)
      fail("jump link kept stale projection", 1, 0);

   CPU_reg[12] = CPU_reg[9];
   PGXP_CPU_ObserveInstruction(INSTR_RS(8) | INSTR_RD(12) | 0x09u);
   if (CPU_reg[12].flags & VALID_PROJECTION)
      fail("register jump link kept stale projection", 1, 0);
}

static void test_exact_shift_lineage(void)
{
   const float precise_x = 143.25f;
   const float precise_y = -92.5f;
   const uint32_t packed = pack_vertex(143, -92);
   const uint32_t shifted = packed << 5;
   const uint32_t restored = (uint32_t)((int32_t)shifted >> 5);
   const uint32_t projected_addr = 0x80105ffcu;
   const uint32_t shifted_addr = 0x80106000u;
   const uint32_t restored_addr = 0x80106004u;
   PGXP_value partial = PGXP_value_zero;
   PGXP_value overwrite = PGXP_value_zero;
   OGLVertex vertex;
   float x = 0.f;
   float y = 0.f;
   float z = 0.f;
   int valid_w = 0;
   uint32_t instr;

   PGXP_InitCPU();
   PGXP_InitGTE();
   PGXP_EnableModes(PGXP_TEXTURE_CORRECTION);
   PGXP_LineageReset();
   PGXP_pushSXYZ2f(precise_x, precise_y, 32768.f, packed);
   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);

   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(8), packed, projected_addr);
   PGXP_LineageFIFOWrite(3, projected_addr, packed);
   PGXP_LineageCBWrite(3, 3);
   if (PGXP_LineageRecoverVertex(3, packed, &x, &y, &z, &valid_w))
      fail("unshifted lineage reached vertex", 1, 0);

   instr = INSTR_RS(8) | INSTR_RT(0) | INSTR_RD(7) | 0x21u;
   PGXP_CPU_MemoryDispatch(instr, packed, packed, 0);
   PGXP_CPU_ObserveInstruction(instr);

   instr = INSTR_RT(7) | INSTR_RD(9) | INSTR_SA(5);
   PGXP_CPU_MemoryDispatch(instr, shifted, 0, packed);
   PGXP_CPU_ObserveInstruction(instr);
   if ((CPU_reg[9].flags & VALID_01) == VALID_01)
      fail("shift lineage changed CPU shadow", 1, 0);

   instr = INSTR_RS(9) | INSTR_RT(0) | INSTR_RD(11) | 0x21u;
   PGXP_CPU_MemoryDispatch(instr, shifted, shifted, 0);
   PGXP_CPU_ObserveInstruction(instr);
   if ((CPU_reg[11].flags & VALID_01) == VALID_01)
      fail("shift identity changed CPU shadow", 1, 0);
   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(11), shifted, shifted_addr);
   PGXP_LineageFIFOWrite(0, shifted_addr, shifted);
   PGXP_LineageCBWrite(0, 0);
   if (!PGXP_LineageRecoverVertex(0, shifted, &x, &y, &z, &valid_w) ||
       x != precise_x || y != precise_y || z != 32768.f || !valid_w)
      fail("shifted lineage recovery missed", 0, 1);

   PGXP_CPU_LW(INSTR_OP(0x23) | INSTR_RT(10), shifted, shifted_addr);
   PGXP_CPU_ObserveInstruction(INSTR_OP(0x23) | INSTR_RT(10));
   instr = INSTR_RT(10) | INSTR_RD(10) | INSTR_SA(5) | 0x03u;
   PGXP_CPU_MemoryDispatch(instr, restored, 0, shifted);
   PGXP_CPU_ObserveInstruction(instr);

   instr = INSTR_RS(10) | INSTR_RT(0) | INSTR_RD(12) | 0x21u;
   PGXP_CPU_MemoryDispatch(instr, restored, restored, 0);
   PGXP_CPU_ObserveInstruction(instr);
   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(12), restored, restored_addr);
   PGXP_LineageFIFOWrite(1, restored_addr, restored);
   PGXP_LineageCBWrite(1, 1);
   if (!PGXP_LineageRecoverVertex(1, restored, &x, &y, &z, &valid_w) ||
       x != precise_x || y != precise_y || z != 32768.f || !valid_w)
      fail("restored lineage recovery missed", 0, 1);
   memset(&vertex, 0, sizeof(vertex));
   PGXP_GetVertex(1, &restored, &vertex, 0, 0);
   if (vertex.x != precise_x || vertex.y != precise_y ||
       vertex.w != 32768.f || !vertex.valid_w)
      fail("vertex lookup ignored exact lineage", 0, 1);
   if (PGXP_LineageRecoverVertex(1, restored ^ 1u,
       &x, &y, &z, &valid_w))
      fail("lineage accepted mismatched word", 1, 0);

   partial.value = restored;
   WriteMem16(&partial, restored_addr);
   PGXP_LineageFIFOWrite(2, restored_addr, restored);
   PGXP_LineageCBWrite(2, 2);
   if (PGXP_LineageRecoverVertex(2, restored, &x, &y, &z, &valid_w))
      fail("partial overwrite kept lineage", 1, 0);

   overwrite.value = shifted;
   WriteMem(&overwrite, shifted_addr);
   PGXP_LineageFIFOWrite(4, shifted_addr, shifted);
   PGXP_LineageCBWrite(4, 4);
   if (PGXP_LineageRecoverVertex(4, shifted, &x, &y, &z, &valid_w))
      fail("full overwrite kept lineage", 1, 0);
}

static int recover_tagged_lineage(uint32_t packed, uint32_t shifted,
                                  uint32_t tagged, uint32_t restored,
                                  int restore)
{
   const uint32_t addr = 0x80106020u;
   uint32_t output = tagged;
   uint32_t output_addr = addr;
   unsigned output_reg = 9;
   uint32_t instr;
   float x = 0.f;
   float y = 0.f;
   float z = 0.f;
   int valid_w = 0;

   PGXP_LineageReset();
   PGXP_pushSXYZ2f(143.25f, -92.5f, 32768.f, packed);
   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);
   instr = INSTR_RT(8) | INSTR_RD(9) | INSTR_SA(5);
   PGXP_CPU_MemoryDispatch(instr, shifted, 0, packed);
   PGXP_CPU_ObserveInstruction(instr);
   instr = INSTR_OP(0x08) | INSTR_RS(9) | INSTR_RT(9) |
      ((tagged - shifted) & 0xffffu);
   PGXP_CPU_MemoryDispatch(instr, tagged, shifted, 0);
   PGXP_CPU_ObserveInstruction(instr);
   if (restore)
   {
      PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(9), tagged, addr);
      PGXP_CPU_LW(INSTR_OP(0x23) | INSTR_RT(10), tagged, addr);
      PGXP_CPU_ObserveInstruction(INSTR_OP(0x23) | INSTR_RT(10));
      instr = INSTR_RT(10) | INSTR_RD(10) | INSTR_SA(5) | 0x03u;
      PGXP_CPU_MemoryDispatch(instr, restored, 0, tagged);
      PGXP_CPU_ObserveInstruction(instr);
      output = restored;
      output_addr = addr + 4;
      output_reg = 10;
   }
   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(output_reg),
      output, output_addr);
   PGXP_LineageFIFOWrite(0, output_addr, output);
   PGXP_LineageCBWrite(0, 0);
   return PGXP_LineageRecoverVertex(0, output,
      &x, &y, &z, &valid_w) && x == 143.25f && y == -92.5f &&
      z == 32768.f && valid_w;
}

static void test_tagged_shift_lineage(void)
{
   const uint32_t packed = pack_vertex(143, -92);
   const uint32_t shifted = packed << 5;
   const uint32_t tagged = shifted + 8;
   const uint32_t restored = (uint32_t)((int32_t)tagged >> 5);
   const uint32_t lossy_packed = pack_vertex(143, 2048);
   const uint32_t lossy_shifted = lossy_packed << 5;
   const uint32_t lossy_restored =
      (uint32_t)((int32_t)lossy_shifted >> 5);

   if (!recover_tagged_lineage(packed, shifted, tagged, restored, 1))
      fail("guarded tag transport missed exact round trip", 0, 1);
   if (recover_tagged_lineage(packed, shifted, tagged, restored, 0))
      fail("tag transport accepted an unrestored value", 1, 0);
   if (recover_tagged_lineage(packed, shifted, shifted + 32,
       (uint32_t)((int32_t)(shifted + 32) >> 5), 1))
      fail("tag transport accepted upper-bit change", 1, 0);
   if (recover_tagged_lineage(lossy_packed, lossy_shifted,
       lossy_shifted + 8, lossy_restored, 1))
      fail("tag transport accepted lossy shift round trip", 1, 0);
}

static void store_shifted_lineage(uint32_t addr, uint32_t packed,
                                  uint32_t shifted)
{
   uint32_t instr;

   PGXP_pushSXYZ2f(143.25f, -92.5f, 32768.f, packed);
   instr = INSTR_RT(8) | INSTR_RD(14) | INSTR_OP(0x12);
   PGXP_GTE_MFC2(instr, packed, packed);
   PGXP_CPU_ObserveInstruction(instr);
   instr = INSTR_RT(8) | INSTR_RD(9) | INSTR_SA(5);
   PGXP_CPU_MemoryDispatch(instr, shifted, 0, packed);
   PGXP_CPU_ObserveInstruction(instr);
   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(9), shifted, addr);
}

static int recover_shifted_lineage(uint32_t addr, uint32_t shifted)
{
   float x = 0.f;
   float y = 0.f;
   float z = 0.f;
   int valid_w = 0;

   PGXP_LineageFIFOWrite(0, addr, shifted);
   PGXP_LineageCBWrite(0, 0);
   return PGXP_LineageRecoverVertex(0, shifted,
      &x, &y, &z, &valid_w) && x == 143.25f && y == -92.5f &&
      z == 32768.f && valid_w;
}

static void test_lineage_memory_addressing(void)
{
   const uint32_t packed = pack_vertex(143, -92);
   const uint32_t shifted = packed << 5;
   const uint32_t base = 0x80106000u;
   const uint32_t mirror = 0x80306000u;
   PGXP_value shadow = PGXP_value_zero;
   PGXP_value overwrite = PGXP_value_zero;

   shadow.value = shifted;
   shadow.x = 143.25f;
   overwrite.value = shifted ^ 1u;
   overwrite.x = -1.f;
   WriteMem(&shadow, base);
   WriteMem(&overwrite, 0x80906000u);
   if (ReadMem(0x80906000u) || !ReadMem(base) ||
       ReadMem(base)->value != shifted || ReadMem(base)->x != 143.25f)
      fail("unmapped address aliased shadow RAM", 1, 0);

   PGXP_LineageReset();
   store_shifted_lineage(base, packed, shifted);
   if (!recover_shifted_lineage(mirror, shifted))
      fail("RAM mirror lost lineage", 0, 1);

   PGXP_LineageMemoryWrite(0x00906000u);
   PGXP_LineageMemoryWrite(0x80906000u);
   PGXP_LineageMemoryWrite(0xC0106000u);
   if (!recover_shifted_lineage(base, shifted))
      fail("unmapped address aliased RAM", 1, 0);

   PGXP_LineageMemoryWrite(mirror);
   if (recover_shifted_lineage(base, shifted))
      fail("RAM mirror failed to invalidate", 1, 0);

   store_shifted_lineage(0x80906000u, packed, shifted);
   if (recover_shifted_lineage(base, shifted))
      fail("unmapped store created RAM lineage", 1, 0);

   store_shifted_lineage(base, packed, shifted);
   PGXP_CPU_SW(INSTR_OP(0x2b) | INSTR_RT(9), shifted, base + 4);
   PGXP_LineageMemoryWriteRange(base + 3, 2);
   if (recover_shifted_lineage(base, shifted) ||
       recover_shifted_lineage(base + 4, shifted))
      fail("cross-word range kept lineage", 1, 0);

   store_shifted_lineage(0x1F800020u, packed, shifted);
   if (!recover_shifted_lineage(0xBF800020u, shifted))
      fail("scratchpad alias lost lineage", 0, 1);
   PGXP_LineageMemoryWriteRange(0x9F800021u, 2);
   if (recover_shifted_lineage(0x1F800020u, shifted))
      fail("scratchpad alias kept lineage", 1, 0);
}

static void test_subword_store_lineage_invalidation(void)
{
   const uint32_t packed = pack_vertex(143, -92);
   const uint32_t shifted = packed << 5;
   const uint32_t addr = 0x80106000u;

   PGXP_LineageReset();
   store_shifted_lineage(addr, packed, shifted);
   if (!recover_shifted_lineage(addr, shifted))
      fail("SB lineage setup failed", 0, 1);
   PGXP_CPU_SB(INSTR_OP(0x28) | INSTR_RT(9),
      (uint8_t)shifted, addr + 1);
   if (recover_shifted_lineage(addr, shifted))
      fail("SB kept exact lineage", 1, 0);

   store_shifted_lineage(addr, packed, shifted);
   if (!recover_shifted_lineage(addr, shifted))
      fail("SH lineage setup failed", 0, 1);
   PGXP_CPU_SH(INSTR_OP(0x29) | INSTR_RT(9),
      (uint16_t)shifted, addr + 2);
   if (recover_shifted_lineage(addr, shifted))
      fail("SH kept exact lineage", 1, 0);
}

static void test_lineage_allocation_lifetime(void)
{
   const uint32_t packed = pack_vertex(143, -92);
   const uint32_t shifted = packed << 5;
   const uint32_t addr = 0x80106000u;

   PGXP_LineageReset();
   store_shifted_lineage(addr, packed, shifted);
   if (!recover_shifted_lineage(addr, shifted))
      fail("allocated lineage unavailable", 0, 1);

   PGXP_SetModes(PGXP_MODE_NONE);
   PGXP_LineageMemoryWriteRange(addr, 4);
   if (recover_shifted_lineage(addr, shifted))
      fail("disabled lineage remained live", 1, 0);

   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_TEXTURE_CORRECTION);
   if (recover_shifted_lineage(addr, shifted))
      fail("reallocated lineage retained data", 1, 0);
   store_shifted_lineage(addr, packed, shifted);
   if (!recover_shifted_lineage(addr, shifted))
      fail("reallocated lineage unavailable", 0, 1);
}

static void test_zero_register_writes(void)
{
   const uint32_t addu = INSTR_RS(8) | INSTR_RT(9) |
      INSTR_RD(0) | 0x21u;
   const uint32_t addiu = INSTR_OP(0x09) | INSTR_RS(8) | INSTR_RT(0) | 1u;

   CPU_reg[0] = PGXP_value_zero;
   CPU_reg[8] = PGXP_value_zero;
   CPU_reg[9] = PGXP_value_zero;
   CPU_reg[8].x = 1.25f;
   CPU_reg[8].value = 1;
   CPU_reg[9].x = 2.5f;
   CPU_reg[9].value = 2;

   /* Beetle's interpreter calls the tracker directly, then the common
    * observer at the end of the instruction. */
   PGXP_CPU_ADDU(addu, 3, 1, 2);
   PGXP_CPU_ObserveInstruction(addu);
   if (memcmp(&CPU_reg[0], &PGXP_value_zero, sizeof(CPU_reg[0])) != 0)
      fail("direct r0 write changed shadow", CPU_reg[0].value, 0);

   /* Both Lightrec engines enter through the shared dispatcher. */
   PGXP_CPU_Dispatch(addiu, 2, 1, 0, 0, 0, 0);
   PGXP_CPU_ObserveInstruction(addiu);
   if (memcmp(&CPU_reg[0], &PGXP_value_zero, sizeof(CPU_reg[0])) != 0)
      fail("dispatched r0 write changed shadow", CPU_reg[0].value, 0);
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

   printf("[T6] CPU result initialization\n");
   test_cpu_result_initialization();

   printf("[T7] CPU comparison ordering\n");
   test_cpu_comparison_ordering();

   printf("[T8] native-zero ADD identity\n");
   test_native_zero_add_identity();

   printf("[T9] projection provenance\n");
   test_projection_provenance();

   printf("[T10] NCLIP native magnitude preservation\n");
   test_nclip_magnitude();

   printf("[T11] projected-depth CPU transport\n");
   test_projection_provenance_transport();

   printf("[T12] projected-depth identity moves\n");
   test_projection_identity_moves();

   printf("[T13] memory-mode CPU dispatch\n");
   test_memory_mode_dispatch();

   printf("[T14] projected-depth write observation\n");
   test_projection_write_observer();

   printf("[T15] exact projected-coordinate shift lineage\n");
   test_exact_shift_lineage();

   printf("[T16] lineage address decoding and range invalidation\n");
   test_lineage_memory_addressing();

   printf("[T17] lineage allocation lifetime\n");
   test_lineage_allocation_lifetime();

   printf("[T18] architectural zero-register writes\n");
   test_zero_register_writes();

   printf("[T19] sub-word store lineage invalidation\n");
   test_subword_store_lineage_invalidation();

   printf("[T20] guarded low-bit tagged shift lineage\n");
   test_tagged_shift_lineage();

   if (failures)
   {
      PGXP_Shutdown();
      printf("\nfailures=%d\nFAIL\n", failures);
      return 1;
   }
   PGXP_Shutdown();
   printf("\nFAIL count 0\nPASS\n");
   return 0;
}
