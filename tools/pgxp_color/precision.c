/* Regression checks for the real PGXP arithmetic and GPU consumers. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../pgxp/pgxp_cpu.h"
#include "../../pgxp/pgxp_gpu.h"
#include "../../pgxp/pgxp_gte.h"
#include "../../pgxp/pgxp_main.h"
#include "../../pgxp/pgxp_mem.h"
#include "../../pgxp/pgxp_value.h"

static unsigned checks;
static unsigned failures;
static uint32_t random_state = 0x51f2e793u;
extern uint32_t pgxp_test_sz3;

static void check(int condition, const char *name)
{
   checks++;
   if (!condition)
   {
      if (failures < 40)
         fprintf(stderr, "FAIL %s\n", name);
      failures++;
   }
}

static uint32_t random_word(void)
{
   random_state ^= random_state << 13;
   random_state ^= random_state >> 17;
   random_state ^= random_state << 5;
   return random_state;
}

static uint32_t rtype(unsigned rs, unsigned rt, unsigned rd, unsigned fn)
{
   return (rs << 21) | (rt << 16) | (rd << 11) | fn;
}

static uint32_t itype(unsigned rs, unsigned rt, uint16_t immediate)
{
   return (rs << 21) | (rt << 16) | immediate;
}

static void seed(unsigned reg, uint32_t value, float x, float y, float z)
{
   CPU_reg[reg] = PGXP_value_zero;
   SetValue(&CPU_reg[reg], value);
   CPU_reg[reg].x = x;
   CPU_reg[reg].y = y;
   CPU_reg[reg].z = z;
   CPU_reg[reg].flags = VALID_012;
}

static void test_vertex_fallback(void)
{
   uint32_t word = 0x0022000cu;
   PGXP_value shadow = PGXP_value_zero;
   OGLVertex vertex;
   PGXP_SetModes(PGXP_MODE_MEMORY);
   memset(&vertex, 0xa5, sizeof(vertex));
   PGXP_WriteCB(&shadow, 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.f && vertex.y == 34.f && !vertex.valid_w,
         "native vertex fallback coordinates");
   check(vertex.z == 0.95f && vertex.w == 1.f,
         "native vertex fallback initializes unused depth");

   seed(1, word, 12.25f, 34.75f, 8.f);
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.25f && vertex.y == 34.75f && vertex.w == 8.f && vertex.valid_w,
         "valid vertex keeps precision and depth");
   CPU_reg[1].flags = VALID_01;
   CPU_reg[1].z = 0.f;
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.25f && vertex.y == 34.75f && !vertex.valid_w,
         "XY-only vertex keeps XY and refuses depth");
}

static void check_stored_vertex(uint32_t addr, float w, const char *name)
{
   uint32_t word = 0x0022000cu;
   OGLVertex vertex;
   PGXP_WriteCB(ReadMem(addr), 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.25f && vertex.y == 34.75f &&
         vertex.valid_w && vertex.w == w, name);
}

static void test_stored_depth(void)
{
   const uint32_t modes = PGXP_MODE_MEMORY | PGXP_MODE_CPU |
         PGXP_MODE_GTE | PGXP_TEXTURE_CORRECTION;
   const uint32_t xy = 0x0022000cu;
   const uint32_t record = 0x1f800000u;
   const uint32_t mfc2 = rtype(0, 2, 19, 0);
   const uint32_t srl4 = rtype(0, 2, 2, 2) | (4u << 6);
   const uint32_t sw = (0x2bu << 26) | itype(0, 2, 4);
   const float raw_w = 4097.5f;
   unsigned i;

   PGXP_SetModes(modes | PGXP_NORMALIZE_STORED_DEPTH);
   pgxp_test_sz3 = 4097;
   for (i = 0; i < 6; i++)
   {
      PGXP_pushSXYZ2f(12.25f, 34.75f, raw_w, xy);
      PGXP_GTE_SWC2(itype(0, 14, 0), xy, record);
      if (i == 1)
         PGXP_pushSXYZ2f(12.25f, 34.75f, raw_w, xy);
      if (i == 2)
      {
         PGXP_GTE_MFC2(mfc2, 4097, 4097);
         PGXP_GTE_MTC2(mfc2, 4097, 4097);
      }
      PGXP_GTE_ReadDelayed(mfc2, 4097);
      PGXP_CPU_CommitLoad(2, 4097, 0);
      if (i == 3)
      {
         PGXP_CPU_SW(sw, 4097, record + 4);
         check_stored_vertex(record, raw_w, "unscaled SZ leaves projected depth intact");
         continue;
      }
      PGXP_CPU_SRL(srl4, 256, 4097);
      PGXP_CPU_ORI(itype(2, 2, 0xa000), 0xa100, 256);
      if (i == 4)
      {
         PGXP_CPU_SW(sw, 0xa100, record + 20);
         PGXP_CPU_SH(itype(0, 2, 20), 0xa100, record + 20);
         PGXP_CPU_LW(itype(0, 2, 20), 0xa100, record + 20);
         check(!(CPU_reg[2].flags & PGXP_DEPTH_TAGS),
               "partial SZ overwrite retires scalar ownership");
      }
      if (i == 5)
         PGXP_GTE_SWC2(itype(0, 14, 0), xy, 0x10000);
      PGXP_CPU_SW(sw, 0xa100, i == 5 ? 0x10004 : record + 4);
      check_stored_vertex(i == 5 ? 0x10000 : record,
            i == 0 ? raw_w / 16.0f : raw_w,
            "renderer uses guest depth scale only for the matching scratchpad projection");
      if (i == 0)
      {
         PGXP_CPU_SW(sw, 0xa100, record + 4);
         check_stored_vertex(record, raw_w / 16.0f, "paired store applies scale once");
      }
   }
   PGXP_SetModes(modes);
   check(!(ReadMem(record)->flags & VALID_012), "depth-policy transition retires scratchpad shadow");
   PGXP_pushSXYZ2f(12.25f, 34.75f, raw_w, xy);
   PGXP_GTE_SWC2(itype(0, 14, 0), xy, record);
   PGXP_GTE_MFC2(mfc2, 4097, 4097);
   PGXP_CPU_SRL(srl4, 256, 4097);
   PGXP_CPU_SW(sw, 256, record + 4);
   check_stored_vertex(record, raw_w, "disabled normalization keeps raw depth");
}

static void test_comparisons(void)
{
   unsigned i;
   for (i = 0; i < 20000; i++)
   {
      uint32_t a = random_word(), b = random_word();
      uint16_t imm = (uint16_t)random_word();
      unsigned signed_result = (int32_t)a < (int32_t)b;
      unsigned unsigned_result = a < b;
      SetValue(&CPU_reg[1], a);
      SetValue(&CPU_reg[2], b);
      PGXP_CPU_SLT(rtype(1, 2, 3, 0x2a), signed_result, a, b);
      check(CPU_reg[3].x == (float)signed_result, "SLT compares both halfwords");
      SetValue(&CPU_reg[1], a);
      SetValue(&CPU_reg[2], b);
      PGXP_CPU_SLTU(rtype(1, 2, 3, 0x2b), unsigned_result, a, b);
      check(CPU_reg[3].x == (float)unsigned_result, "SLTU compares both halfwords");
      signed_result = (int32_t)a < (int32_t)(int16_t)imm;
      unsigned_result = a < (uint32_t)(int32_t)(int16_t)imm;
      SetValue(&CPU_reg[1], a);
      PGXP_CPU_SLTI(itype(1, 3, imm), signed_result, a);
      check(CPU_reg[3].x == (float)signed_result, "SLTI compares sign-extended immediate");
      SetValue(&CPU_reg[1], a);
      PGXP_CPU_SLTIU(itype(1, 3, imm), unsigned_result, a);
      check(CPU_reg[3].x == (float)unsigned_result, "SLTIU compares sign-extended immediate");
   }
   seed(1, 0x00020001u, 1.25f, 2.f, 8.f);
   seed(2, 0x00010002u, 2.75f, 1.f, 8.f);
   PGXP_CPU_SLT(rtype(1, 2, 1, 0x2a), 0, 0x00020001u, 0x00010002u);
   check(CPU_reg[1].x == 0.f, "comparison with aliased destination");
}

static void test_hilo_moves(void)
{
   uint32_t word = 0x0022000cu;
   unsigned fn;
   for (fn = 0x11; fn <= 0x13; fn += 2)
   {
      OGLVertex vertex = {0};
      seed(7, word, 12.25f, 34.75f, 8.f);
      if (fn == 0x11)
      {
         PGXP_CPU_MTHI(rtype(7, 0, 0, fn), word, word);
         PGXP_CPU_MFHI(rtype(0, 0, 8, 0x10), word, word);
      }
      else
      {
         PGXP_CPU_MTLO(rtype(7, 0, 0, fn), word, word);
         PGXP_CPU_MFLO(rtype(0, 0, 8, 0x12), word, word);
      }
      PGXP_CPU_SW(itype(0, 8, 0), word, 0x10000);
      PGXP_WriteCB(ReadMem(0x10000), 0);
      PGXP_GetVertex(0, &word, &vertex, 0, 0);
      check(vertex.x == 12.25f && vertex.y == 34.75f && vertex.w == 8.f && vertex.valid_w,
            "HI/LO roundtrip reaches actual vertex consumer");
   }
}

static void test_fractional_boundaries(void)
{
   seed(1, 0x0001ffffu, -0.25f, 1.f, 8.f);
   PGXP_CPU_ADDIU(itype(1, 3, 0), 0x0001ffffu, 0x0001ffffu);
   check(CPU_reg[3].x == -0.25f && CPU_reg[3].y == 1.f,
         "ADDIU fractional low half does not carry early");
   seed(1, 0x7fff0001u, 1.f, 32767.5f, 8.f);
   PGXP_CPU_ADDIU(itype(1, 3, 0), 0x7fff0001u, 0x7fff0001u);
   check(CPU_reg[3].y == 32767.5f, "signed high half does not wrap early");
   seed(1, 0x0000ffffu, -0.25f, 0.f, 8.f);
   seed(2, 1, 1.f, 0.f, 8.f);
   PGXP_CPU_ADDU(rtype(1, 2, 3, 0x21), 0x10000u, 0xffffu, 1);
   check(CPU_reg[3].x == 0.75f && CPU_reg[3].y == 1.f,
         "ADDU real halfword carry retained");
   seed(1, 1, 1.f, 0.f, 8.f);
   seed(2, 2, 2.f, 0.f, 8.f);
   PGXP_CPU_SUBU(rtype(1, 2, 3, 0x23), 0xffffffffu, 1, 2);
   check(CPU_reg[3].x == -1.f && CPU_reg[3].y == -1.f,
         "SUBU real halfword borrow retained");
}

static void test_halfword_extension(void)
{
   PGXP_value value = PGXP_value_zero;
   SetValue(&value, 0);
   value.x = -0.25f;
   value.y = 20.5f;
   value.z = 8.f;
   value.flags = VALID_012;
   WriteMem(&value, 0x10000);
   PGXP_CPU_LH(itype(0, 4, 0), 0, 0x10000);
   check(CPU_reg[4].x == -0.25f && CPU_reg[4].y == 0.f,
         "LH sign extension follows native sign bit");
   value.value = 0x0000ffffu;
   value.x = -0.75f;
   WriteMem(&value, 0x10000);
   PGXP_CPU_LH(itype(0, 4, 0), 0xffffu, 0x10000);
   check(CPU_reg[4].y == -1.f, "LH negative native half remains signed");
   PGXP_CPU_LHU(itype(0, 4, 0), 0xffffu, 0x10000);
   check(CPU_reg[4].y == 0.f, "LHU remains unsigned");
}

static void test_division(void)
{
   unsigned unsigned_op;
   for (unsigned_op = 0; unsigned_op < 2; unsigned_op++)
   {
      seed(1, 7, 7.25f, 0.f, 8.f);
      SetValue(&CPU_reg[2], 0);
      if (unsigned_op)
         PGXP_CPU_DIVU(rtype(1, 2, 0, 0x1b), 7, 0xffffffffu, 7, 0);
      else
         PGXP_CPU_DIV(rtype(1, 2, 0, 0x1a), 7, 0xffffffffu, 7, 0);
      check(isfinite(CPU_reg[32].x) && isfinite(CPU_reg[32].y) &&
            isfinite(CPU_reg[33].x) && isfinite(CPU_reg[33].y),
            "division by zero has finite canonical fallback");
      check(CPU_Hi.x == 7.f && CPU_Hi.y == 0.f &&
            CPU_Lo.x == -1.f && CPU_Lo.y == -1.f,
            "division by zero uses architectural quotient and remainder");
   }
}

static void test_reset_and_modes(void)
{
   uint32_t word = 0x0022000cu;
   OGLVertex vertex = {0};
   seed(1, word, 12.25f, 34.75f, 8.f);
   PGXP_WriteFIFO(&CPU_reg[1], 0);
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_Init();
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.f && vertex.y == 34.f && !vertex.valid_w,
         "reset invalidates command-buffer precision");
   check(!(PGXP_ReadFIFO(0)->flags & VALID_012), "reset invalidates FIFO precision");

   seed(1, word, 12.25f, 34.75f, 8.f);
   WriteMem(&CPU_reg[1], 0x10000);
   PGXP_SetModes(0);
   PGXP_SetModes(PGXP_MODE_MEMORY);
   PGXP_CPU_LW(itype(0, 2, 0), word, 0x10000);
   PGXP_WriteCB(&CPU_reg[2], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.f && vertex.y == 34.f && !vertex.valid_w,
         "off-to-on refuses pre-disable memory precision with equal native word");
   seed(1, word, 12.25f, 34.75f, 8.f);
   WriteMem(&CPU_reg[1], 0x10000);
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_TEXTURE_CORRECTION | PGXP_DEPTH_BUFFER);
   PGXP_CPU_LW(itype(0, 2, 0), word, 0x10000);
   PGXP_WriteCB(&CPU_reg[2], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.f && vertex.y == 34.f && !vertex.valid_w,
         "projection-policy change retires old precision through the consumer");
}

static void test_texture_modes(void)
{
   uint32_t word = 0x0022000cu;
   uint32_t modes = PGXP_MODE_MEMORY | PGXP_MODE_GTE;
   OGLVertex vertex = {0};
   PGXP_Init();
   PGXP_SetModes(modes);
   PGXP_pushSXYZ2f(12.25f, 34.75f, 8.f, word);
   WriteMem(&GTE_data_reg[14], 0x10000);
   PGXP_WriteCB(&GTE_data_reg[14], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.w == 1.f && vertex.valid_w,
         "texture-off producer supplies unit depth");

   PGXP_SetModes(modes | PGXP_TEXTURE_CORRECTION);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(!vertex.valid_w, "texture-on refuses old command-buffer unit depth");
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(!vertex.valid_w, "texture-on refuses old memory unit depth");

   PGXP_pushSXYZ2f(12.25f, 34.75f, 8.f, word);
   WriteMem(&GTE_data_reg[14], 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.w == 8.f && vertex.valid_w, "fresh texture-on depth reaches consumer");
   PGXP_SetModes(modes | PGXP_TEXTURE_CORRECTION | PGXP_NCLIP_IMPL);
   PGXP_SetModes(modes | PGXP_TEXTURE_CORRECTION | PGXP_NCLIP_IMPL);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.w == 8.f && vertex.valid_w,
         "unchanged producer policy and NCLIP preserve live depth");

   PGXP_SetModes(modes);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(!vertex.valid_w, "texture-off refuses old perspective depth");
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
}

static void test_gte_alias(void)
{
   uint32_t word = 0x0022000cu;
   uint32_t instruction = (8u << 16) | (15u << 11);
   OGLVertex vertex = {0};
   PGXP_pushSXYZ2f(12.25f, 34.75f, 8.f, word);
   PGXP_GTE_MFC2(instruction, word, word);
   PGXP_WriteCB(&CPU_reg[8], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.25f && vertex.y == 34.75f,
         "MFC2 SXYP reads the current SXY2 precision");
   PGXP_GTE_SWC2(15u << 16, word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 12.25f && vertex.y == 34.75f,
         "SWC2 SXYP reads the current SXY2 precision");
   instruction = (8u << 16) | (14u << 11);
   PGXP_GTE_MFC2(instruction, word, word);
   check(CPU_reg[8].x == 12.25f && CPU_reg[8].y == 34.75f,
         "direct SXY2 read retains precision");
}

static void test_shifts(void)
{
   unsigned sh, i;
   for (sh = 0; sh < 32; sh++)
   {
      for (i = 0; i < 256; i++)
      {
         uint32_t value = random_word();
         uint32_t result = value >> sh;
         unsigned low_shift = sh < 16 ? sh : sh - 16;
         uint32_t low_mask = (1u << low_shift) - 1;
         double fraction_x = ((sh < 16 ? value : value >> 16) & low_mask) /
               (double)(1u << low_shift);
         double fraction_y = sh < 16 ? ((value >> 16) & ((1u << sh) - 1)) /
               (double)(1u << sh) : 0.0;
         SetValue(&CPU_reg[1], value);
         PGXP_CPU_SRL(rtype(0, 1, 3, 2) | (sh << 6), result, value);
         check(CPU_reg[3].x == (float)((int16_t)result + fraction_x) &&
               CPU_reg[3].y == (float)((int16_t)(result >> 16) + fraction_y),
               "SRL integer input retains architectural halfwords");
         SetValue(&CPU_reg[1], value);
         SetValue(&CPU_reg[2], sh);
         PGXP_CPU_SRLV(rtype(2, 1, 3, 6), result, value, sh);
         check(CPU_reg[3].x == (float)((int16_t)result + fraction_x) &&
               CPU_reg[3].y == (float)((int16_t)(result >> 16) + fraction_y),
               "SRLV matches fixed shift reference");
         result = (uint32_t)((int32_t)value >> sh);
         if (sh && ((result >> 16) == 0 || (result >> 16) == 0xffffu))
            fraction_y = 0.0;
         SetValue(&CPU_reg[1], value);
         PGXP_CPU_SRA(rtype(0, 1, 3, 3) | (sh << 6), result, value);
         check(CPU_reg[3].x == (float)((int16_t)result + fraction_x) &&
               CPU_reg[3].y == (float)((int16_t)(result >> 16) + fraction_y),
               "SRA integer input retains architectural halfwords");
         SetValue(&CPU_reg[1], value);
         SetValue(&CPU_reg[2], sh);
         PGXP_CPU_SRAV(rtype(2, 1, 3, 7), result, value, sh);
         check(CPU_reg[3].x == (float)((int16_t)result + fraction_x) &&
               CPU_reg[3].y == (float)((int16_t)(result >> 16) + fraction_y),
               "SRAV matches fixed shift reference");
         result = value << sh;
         SetValue(&CPU_reg[1], value);
         PGXP_CPU_SLL(rtype(0, 1, 3, 0) | (sh << 6), result, value);
         check(CPU_reg[3].x == (int16_t)result && CPU_reg[3].y == (int16_t)(result >> 16),
               "SLL integer reference");
      }
   }
   seed(1, 0x0022000cu, 12.25f, 34.75f, 8.f);
   PGXP_CPU_SLL(rtype(0, 1, 3, 0) | (16u << 6), 0x000c0000u, 0x0022000cu);
   PGXP_CPU_SRA(rtype(0, 3, 4, 3) | (16u << 6), 12, 0x000c0000u);
   check(CPU_reg[4].x == 12.25f, "SLL/SRA halfword unpack retains fractional X");
   seed(1, 0x0022000cu, 12.25f, 34.75f, 8.f);
   PGXP_CPU_SRL(rtype(0, 1, 3, 2) | (16u << 6), 34, 0x0022000cu);
   check(CPU_reg[3].x == 34.75f, "SRL halfword unpack retains fractional Y");
   seed(1, 0x0022000cu, 12.25f, 34.75f, 8.f);
   CPU_reg[1].flags = VALID_0 | VALID_2;
   PGXP_CPU_SLL(rtype(0, 1, 3, 0) | (16u << 6), 0x000c0000u, 0x0022000cu);
   check((CPU_reg[3].flags & VALID_01) == VALID_01 && CPU_reg[3].y == 12.25f,
         "left halfword move transfers validity with its component");
}

static void test_shift_sign_extension(void)
{
   static const uint32_t words[] = {0x00100200u, 0xffeffe00u};
   unsigned i, variable;
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   for (i = 0; i < 2; i++)
      for (variable = 0; variable < 2; variable++)
      {
         uint32_t word = words[i];
         uint32_t result = (uint32_t)((int32_t)word >> 14);
         float sign = i ? -1.f : 0.f;
         OGLVertex vertex = {0};
         seed(1, word, (int16_t)word + 0.25f,
               (int16_t)(word >> 16) + 0.5f, 8.f);
         SetValue(&CPU_reg[2], 14);
         if (variable)
            PGXP_CPU_SRAV(rtype(2, 1, 1, 7), result, word, 14);
         else
            PGXP_CPU_SRA(rtype(0, 1, 1, 3) | (14u << 6), result, word);
         check(CPU_reg[1].y == sign && CPU_reg[1].z == 8.f,
               "arithmetic shift replaces exhausted upper half with sign extension");
         PGXP_CPU_SW(itype(0, 1, 0), result, 0x10000);
         PGXP_WriteCB(ReadMem(0x10000), 0);
         PGXP_GetVertex(0, &result, &vertex, 0, 0);
         check(vertex.y == sign && vertex.valid_w && vertex.w == 8.f,
               "vertex consumer receives exact sign extension and retained depth");
      }
   seed(1, 0x00640080u, 128.25f, 100.5f, 8.f);
   PGXP_CPU_SRA(rtype(0, 1, 3, 3) | (1u << 6), 0x00320040u, 0x00640080u);
   check(CPU_reg[3].x == 64.125f && CPU_reg[3].y == 50.25f,
         "non-exhausted arithmetic shift retains independent fractions");
   seed(1, 0x00000080u, 128.25f, 0.5f, 8.f);
   PGXP_CPU_SRA(rtype(0, 1, 3, 3), 0x00000080u, 0x00000080u);
   check(CPU_reg[3].x == 128.25f && CPU_reg[3].y == 0.5f,
         "zero arithmetic shift remains an exact identity");
   PGXP_CPU_SRL(rtype(0, 1, 3, 2) | (1u << 6), 0x00000040u, 0x00000080u);
   check(CPU_reg[3].x == 64.125f && CPU_reg[3].y == 0.25f,
         "logical shift retains its unsigned upper-half fraction");
}

static void test_zero_register(void)
{
   uint32_t word = 0;
   OGLVertex vertex = {0};
   PGXP_Init();
   check(PGXP_value_zero.value == 0 && (PGXP_value_zero.flags & VALID_01) == VALID_01,
         "zero constant has valid zero components and native bits");
   seed(1, 0, 0.75f, 0.25f, 8.f);
   PGXP_CPU_ADDU(rtype(1, 0, 0, 0x21), 0, 0, 0);
   PGXP_CPU_SW(itype(0, 0, 0), 0, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 0.f && vertex.y == 0.f,
         "discarded write to zero cannot attach precision to a later zero store");
   PGXP_CPU_ADDIU(itype(0, 3, 1), 1, 0);
   check(CPU_reg[3].x == 1.f && CPU_reg[3].y == 0.f,
         "zero source remains canonical after discarded precision write");
}

static void test_numeric_limits(void)
{
   unsigned i;
   uint32_t word = 0;
   OGLVertex vertex = {0};
   for (i = 0; i < 10000; i++)
   {
      int64_t fixed = (int64_t)random_word() * 65536 + (random_word() & 0xffffu);
      double expected;
      uint32_t low;
      if (random_word() & 1) fixed = -fixed;
      low = (uint32_t)fixed;
      expected = ((double)low - (low >= 0x80000000u ? 4294967296.0 : 0.0)) / 65536.0;
      check(f16Sign((double)fixed / 65536.0) == expected,
            "fixed-point wrapping matches independent integer remainder");
   }
   check(f16Sign(140737488355328.25) == 0.25, "large positive fixed-point wrap");
   check(f16Sign(-140737488355328.25) == -0.25, "large negative fixed-point wrap");
   check(f16Sign(INFINITY) == 0.0 && f16Sign(NAN) == 0.0,
         "non-finite fixed-point inputs never reach an integer conversion");
   for (i = 47; i < 1024; i++)
   {
      double power = ldexp(1.0, i);
      double step = ldexp(1.0, (int)i - 52);
      uint32_t low = i < 68 ? (uint32_t)(step * 65536.0) : 0;
      double positive = ((double)low - (low >= 0x80000000u ? 4294967296.0 : 0.0)) / 65536.0;
      double negative;
      low = 0u - low;
      negative = ((double)low - (low >= 0x80000000u ? 4294967296.0 : 0.0)) / 65536.0;
      check(f16Sign(power) == 0.0 && f16Sign(-power) == 0.0,
            "large positive and negative binary powers wrap to zero");
      check(f16Sign(nextafter(power, INFINITY)) == positive,
            "positive binary exponent boundary retains its representable remainder");
      check(f16Sign(nextafter(-power, -INFINITY)) == negative,
            "negative binary exponent boundary retains its representable remainder");
   }
   seed(1, word, 32768.25f, -32768.5f, 8.f);
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 0.25f && vertex.y == -0.5f,
         "large finite vertex coordinates retain wrapped fractions");
   CPU_reg[1].x = NAN;
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 0.f && vertex.y == 0.f && !vertex.valid_w,
         "non-finite XY uses native fallback");
   seed(1, word, 0.25f, 0.5f, NAN);
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == 0.25f && vertex.y == 0.5f && vertex.w == 1.f && !vertex.valid_w,
         "non-finite depth keeps finite XY and uses unit-depth fallback");
}

static void test_memory_ownership(void)
{
   static const uint32_t owners[3] = {0x10000,0x1f800100,0x1f801810};
   static const uint32_t holes[3] = {0x810000,0x1f800500,0x1f811810};
   static const uint32_t mirrors[4] = {0x80010000,0xa0610000,0x9f800100,0xbf801810};
   uint32_t word=0x0022000cu;
   PGXP_value value={0}, loaded={0};
   OGLVertex vertex={0};
   unsigned i;
   for(i=0;i<3;i++)
   {
      SetValue(&value,word);value.x=12.25f;value.y=34.75f;value.z=8.f;value.flags=VALID_012;
      WriteMem(&value,owners[i]);
      value.x=12.75f;
      WriteMem(&value,holes[i]);
      PGXP_WriteCB(ReadMem(owners[i]),0);
      PGXP_GetVertex(0,&word,&vertex,0,0);
      check(vertex.x==12.25f,"unmapped writes cannot change another region's precision");
      check(GetPtr(holes[i])==NULL,"unmapped address has no precision owner");
   }
   for(i=0;i<4;i++)
   {
      ValidateAndCopyMem(&loaded,mirrors[i],word);
      check(loaded.x==12.25f && (loaded.flags & VALID_01)==VALID_01,
            "cached and uncached aliases share the physical precision owner");
   }
   check(GetPtr(0xc0010000u)==NULL && GetPtr(0x20010000)==NULL,
         "unmapped segments do not acquire RAM precision");

   PGXP_InitMem();
   check(ReadMem(0x10000)->flags==0 && ReadMem(0x10000)->value==0,
         "lazy pointer access cannot expose an earlier timeline");
   SetValue(&value,12);value.x=12.25f;
   WriteMem16(&value,0x10000);
   loaded=*ReadMem(0x10000);
   check(loaded.x==12.25f && !(loaded.flags & VALID_1) && loaded.y==0.f,
         "halfword write after reset cannot inherit an old opposite half");
   SetValue(&value,0x0022000cu);value.y=34.75f;
   WriteMem(&value,0x10000);
   SetValue(&value,12);value.x=12.25f;
   WriteMem16(&value,0x10000);
   loaded=*ReadMem(0x10000);
   check(loaded.x==12.25f && loaded.y==34.75f && (loaded.flags & VALID_01)==VALID_01,
         "ordinary halfword merge preserves the current opposite half");
}

static void test_color_bitwise(void)
{
   typedef void (*bitwise_op)(uint32_t, uint32_t, uint32_t, uint32_t);
   static const bitwise_op operations[] = {PGXP_CPU_AND, PGXP_CPU_OR, PGXP_CPU_XOR};
   static const unsigned functions[] = {0x24, 0x25, 0x26};
   uint32_t word = 0x30ff2010u;
   float rgb[3], pre[3], far[3], t;
   unsigned operation, reverse;
   PGXP_Init();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU | PGXP_MODE_GTE);
   PGXP_GTE_SetFogContext(8.25f, 16.5f, 4088.f, 24.25f, 48.5f, 4104.f, .5f);
   PGXP_pushRGBf(16.25f, 32.5f, 4096.f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   for (operation = 0; operation < 3; operation++)
   {
      uint32_t mask = operation == 0 ? 0xefffffffu : 0x08000000u;
      uint32_t result = operation == 0 ? word & mask : operation == 1 ? word | mask : word ^ mask;
      for (reverse = 0; reverse < 2; reverse++)
      {
         PGXP_CPU_LW(itype(0, 1, 0), word, 0x10000);
         SetValue(&CPU_reg[2], mask);
         operations[operation](rtype(reverse ? 2 : 1, reverse ? 1 : 2, 3, functions[operation]),
               result, reverse ? mask : word, reverse ? word : mask);
         PGXP_CPU_SW(itype(0, 3, 0), result, 0x10004);
         PGXP_WriteCB(ReadMem(0x10004), 0);
         check(PGXP_GetColor(0, &result, rgb) && rgb[0] == 16.25f &&
               rgb[1] == 32.5f && rgb[2] == 4096.f,
               "command-byte mask preserves exact RGB in either operand order");
         check(PGXP_GetFog(0, &result, pre, far, &t) && pre[2] == 4088.f &&
               far[2] == 4104.f && t == .5f,
               "command-byte transport retains its own fog association");
      }
   }
   for (operation = 0; operation < 2; operation++)
   {
      PGXP_CPU_LW(itype(0, 1, 0), word, 0x10000);
      operations[operation](rtype(1, 1, 1, functions[operation]), word, word, word);
      PGXP_CPU_SW(itype(0, 1, 0), word, 0x10004);
      PGXP_WriteCB(ReadMem(0x10004), 0);
      check(PGXP_GetColor(0, &word, rgb) && rgb[2] == 4096.f,
            "self AND and OR preserve the one RGB owner when destination aliases it");
   }
   PGXP_CPU_XOR(rtype(1, 1, 3, 0x26), 0, word, word);
   PGXP_WriteCB(&CPU_reg[3], 0);
   { uint32_t zero = 0; check(!PGXP_GetColor(0, &zero, rgb), "self XOR is canonical zero, not RGB"); }

   PGXP_CPU_LW(itype(0, 1, 0), word, 0x10000);
   SetValue(&CPU_reg[2], 0x00010000u);
   PGXP_CPU_OR(rtype(1, 2, 3, 0x25), word, word, 0x00010000u);
   PGXP_WriteCB(&CPU_reg[3], 0);
   check(!PGXP_GetColor(0, &word, rgb),
         "a mask touching RGB refuses precision even when saturated native bits do not change");
   SetValue(&CPU_reg[2], 0xfffffffeu);
   PGXP_CPU_AND(rtype(1, 2, 3, 0x24), word, word, 0xfffffffeu);
   PGXP_WriteCB(&CPU_reg[3], 0);
   check(!PGXP_GetColor(0, &word, rgb),
         "a non-neutral AND mask cannot borrow RGB from a coincidentally equal result");

   PGXP_pushRGBf(.25f, .25f, .25f, 0);
   PGXP_GTE_MFC2((2u << 16) | (22u << 11), 0, 0);
   PGXP_CPU_OR(rtype(1, 2, 3, 0x25), word, word, 0);
   PGXP_WriteCB(&CPU_reg[3], 0);
   check(!PGXP_GetColor(0, &word, rgb), "two distinct RGB owners are not an integer mask identity");
   SetValue(&CPU_reg[2], 0x08000000u);
   PGXP_CPU_OR(rtype(1, 2, 3, 0x25), (word ^ 1u) | 0x08000000u, word ^ 1u, 0x08000000u);
   check(!(CPU_reg[1].flags & PGXP_COLOR) && !(CPU_reg[3].flags & PGXP_COLOR),
         "a mismatched native source cannot propagate a stale RGB role");
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(PGXP_GetColor(0, &word, rgb) && rgb[2] == 4096.f,
         "bitwise updates leave the independent original memory owner intact");
}

static void test_memory_only_color(void)
{
   uint32_t word = 0x30ff2010;
   float rgb[3];
   PGXP_Init();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_GTE);
   PGXP_pushRGBf(16.25f, 32.5f, 2047.9375f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(PGXP_GetColor(0, &word, rgb), "Memory Only direct GTE store retains RGB");
   PGXP_CPU_LW(itype(0, 1, 0), word, 0x10000);
   PGXP_WriteCB(&CPU_reg[1], 0);
   check(!PGXP_GetColor(0, &word, rgb), "untracked GPR load cannot establish RGB provenance");
   PGXP_CPU_LoadDelayed((35u << 26) | itype(0, 1, 0), word, 0x10000);
   PGXP_CPU_CommitLoad(1, word, 0);
   PGXP_WriteCB(&CPU_reg[1], 0);
   check(!PGXP_GetColor(0, &word, rgb), "delayed native GPR load has the same RGB boundary");
   PGXP_GTE_MFC2((1u << 16) | (22u << 11), word, word);
   PGXP_WriteCB(&CPU_reg[1], 0);
   check(!PGXP_GetColor(0, &word, rgb), "untracked MFC2 cannot establish RGB provenance");
   PGXP_GTE_ReadDelayed((1u << 16) | (22u << 11), word);
   PGXP_CPU_CommitLoad(1, word, 0);
   PGXP_WriteCB(&CPU_reg[1], 0);
   check(!PGXP_GetColor(0, &word, rgb), "delayed native MFC2 has the same RGB boundary");
   PGXP_GTE_MTC2((1u << 16) | (22u << 11), word, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10004);
   PGXP_WriteCB(ReadMem(0x10004), 0);
   check(!PGXP_GetColor(0, &word, rgb), "GTE roundtrip cannot restore unobserved CPU provenance");
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(PGXP_GetColor(0, &word, rgb), "GPR loads leave the independent memory owner intact");
   PGXP_GTE_LWC2(itype(0, 22, 0), word, 0x10000);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10004);
   PGXP_WriteCB(ReadMem(0x10004), 0);
   check(PGXP_GetColor(0, &word, rgb), "direct memory/GTE roundtrip still preserves RGB");
   PGXP_EnableModes(PGXP_MODE_CPU);
   PGXP_pushRGBf(16.25f, 32.5f, 2047.9375f, word);
   PGXP_GTE_MFC2((1u << 16) | (22u << 11), word, word);
   PGXP_WriteCB(&CPU_reg[1], 0);
   check(PGXP_GetColor(0, &word, rgb), "CPU tracking permits a newly produced RGB GPR owner");
}

static void test_boolean_results(void)
{
   uint32_t result = 1;
   OGLVertex vertex = {0};
   PGXP_Init();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   seed(1, 0x00010000, 0.25f, 1.75f, 8.f);
   seed(2, 0x00010001, 1.25f, 1.25f, 8.f);
   PGXP_CPU_SLT(rtype(1, 2, 3, 0x2a), result, 0x00010000, 0x00010001);
   PGXP_CPU_SW(itype(0, 3, 0), result, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, &result, &vertex, 0, 0);
   check(vertex.x == 1.f && vertex.y == 0.f && !vertex.valid_w,
         "SLT transports the actual Boolean, without inventing depth");
   PGXP_CPU_SLTU(rtype(1, 2, 3, 0x2b), result, 0x00010000, 0x00010001);
   check(CPU_reg[3].x == 1.f && CPU_reg[3].y == 0.f,
         "SLTU cannot reverse its actual result using unrelated fractions");
   seed(1, 0, 0.25f, 0.75f, 8.f);
   PGXP_CPU_SLTI(itype(1, 3, 1), result, 0);
   check(CPU_reg[3].x == 1.f && CPU_reg[3].y == 0.f,
         "SLTI keeps the architectural Boolean for fractional inputs");
   PGXP_CPU_SLTIU(itype(1, 3, 1), result, 0);
   check(CPU_reg[3].x == 1.f && CPU_reg[3].y == 0.f,
         "SLTIU keeps the architectural Boolean for fractional inputs");
}

static void test_payload_domains(void)
{
   typedef void (*immediate_op)(uint32_t, uint32_t, uint32_t);
   static const immediate_op identities[] = {
      PGXP_CPU_ADDIU, PGXP_CPU_ORI, PGXP_CPU_XORI
   };
   static const immediate_op changes[] = {
      PGXP_CPU_ADDIU, PGXP_CPU_ANDI, PGXP_CPU_ORI, PGXP_CPU_XORI,
      PGXP_CPU_SLTI, PGXP_CPU_SLTIU
   };
   uint32_t word = 0x00ffff00u;
   uint32_t changed;
   OGLVertex vertex = {0};
   PGXP_value original;
   float rgb[3];
   unsigned i;
   PGXP_Init();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU | PGXP_MODE_GTE | PGXP_TEXTURE_CORRECTION);
   PGXP_pushSXYZ2f(-256.f, 255.5f, 4096.f, word);
   PGXP_GTE_SWC2(itype(0, 14, 0), word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(!PGXP_GetColor(0, &word, rgb),
         "geometry cannot masquerade as RGB with coincidentally matching bytes");
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == -256.f && vertex.y == 255.5f && vertex.w == 4096.f,
         "geometry still reaches its own consumer");
   PGXP_pushRGBf(0.25f, 255.5f, 4096.f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(PGXP_GetColor(0, &word, rgb) && rgb[0] == 0.25f && rgb[2] == 4096.f,
         "real RGB producer retains fractional and over-white channels");
   PGXP_GetVertex(0, &word, &vertex, 0, 0);
   check(vertex.x == -256.f && vertex.y == 255.f && !vertex.valid_w,
         "RGB cannot supply coordinates or depth");
   PGXP_CPU_LW(itype(0, 1, 0), word, 0x10000);
   original = CPU_reg[1];
   for (i = 0; i < sizeof(identities) / sizeof(identities[0]); i++)
   {
      identities[i](itype(1, 3, 0), word, word);
      PGXP_WriteCB(&CPU_reg[3], 0);
      check(PGXP_GetColor(0, &word, rgb) && rgb[2] == 4096.f,
            "zero-immediate transport preserves the real RGB payload");
   }
   for (i = 0; i < sizeof(changes) / sizeof(changes[0]); i++)
   {
      changed = i == 1 ? (word & 1) : i >= 4 ? 0 : (word | 1);
      changes[i](itype(1, 3, 1), changed, word);
      PGXP_WriteCB(&CPU_reg[3], 0);
      check(!PGXP_GetColor(0, &changed, rgb), "arithmetic cannot inherit a colour role");
      check(!memcmp(&original, &CPU_reg[1], sizeof(original)),
            "arithmetic read preserves its unchanged RGB source");
   }
   PGXP_CPU_ADDU(rtype(1, 0, 3, 0x21), word, word, 0);
   PGXP_WriteCB(&CPU_reg[3], 0);
   check(PGXP_GetColor(0, &word, rgb), "register-zero transport retains RGB");
   PGXP_CPU_LHU(itype(0, 3, 0), (uint16_t)word, 0x10000);
   changed = (uint16_t)word;
   PGXP_WriteCB(&CPU_reg[3], 0);
   check(!PGXP_GetColor(0, &changed, rgb), "halfword load cannot keep a whole RGB payload");
   check(!memcmp(&original, ReadMem(0x10000), sizeof(original)),
         "halfword read does not destroy an unchanged whole RGB owner");
   PGXP_CPU_SH(itype(0, 1, 0), (uint16_t)word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(!PGXP_GetColor(0, &word, rgb), "halfword merge retires whole RGB provenance");
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(PGXP_GetColor(0, &word, rgb), "full RGB write recovers after partial-write fallback");
   Validate(ReadMem(0x10000), word ^ 1u);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(!PGXP_GetColor(0, &word, rgb), "native mismatch invalidates RGB provenance");
   word = 0;
   PGXP_WriteCB(&CPU_reg[0], 0);
   check(!PGXP_GetColor(0, &word, rgb), "literal zero is not a GTE colour payload");
}

static void test_dma_ownership(void)
{
   static const uint32_t starts[] = {0, 1, 30, 31, 32, 33, 63,
      PGXP_RAM_WORDS - 33, PGXP_RAM_WORDS - 32,
      PGXP_RAM_WORDS - 2, PGXP_RAM_WORDS - 1};
   static const uint32_t lengths[] = {0, 1, 2, 31, 32, 33, 64,
      PGXP_RAM_WORDS - 1, PGXP_RAM_WORDS, PGXP_RAM_WORDS + 1, UINT32_MAX};
   PGXP_value color, copied;
   uint32_t word = 0x30ff2010;
   float rgb[3];
   unsigned s, n, reverse, i;
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   PGXP_pushRGBf(16.25f, 32.5f, 2047.9375f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   color = *ReadMem(0x10000);
   for (s = 0; s < sizeof(starts) / sizeof(starts[0]); s++)
      for (n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++)
         for (reverse = 0; reverse < 2; reverse++)
         {
            PGXP_InitMem();
            for (i = 0; i < 192; i++)
            {
               uint32_t index = i < 96 ? i : PGXP_RAM_WORDS - (i - 95);
               WriteMem(&color, index * 4);
            }
            WriteMem(&color, 0x1f800000);
            WriteMem(&color, 0x1f801000);
            PGXP_InvalidateRAM(0x80000000u + starts[s] * 4, lengths[n], reverse);
            for (i = 0; i < 192; i++)
            {
               uint32_t index = i < 96 ? i : PGXP_RAM_WORDS - (i - 95);
               uint32_t distance = reverse ? (starts[s] - index) & (PGXP_RAM_WORDS - 1)
                                           : (index - starts[s]) & (PGXP_RAM_WORDS - 1);
               int retired = distance < lengths[n];
               /* Exercise both copy-only and pointer-based consumers: stale
                * raw shadow bytes deliberately still contain a valid color. */
               ValidateAndCopyMem(&copied, index * 4, word);
               PGXP_WriteCB(&copied, 0);
               check(!!PGXP_GetColor(0, &word, rgb) == !retired,
                     "DMA exact range through copy consumer");
               PGXP_WriteCB(ReadMem(index * 4), 0);
               check(!!PGXP_GetColor(0, &word, rgb) == !retired,
                     "DMA exact range through pointer consumer");
            }
            check(ReadMem(0x1f800000)->flags == color.flags &&
                  ReadMem(0x1f801000)->flags == color.flags,
                  "RAM DMA preserves scratchpad and register owners");
         }
   WriteMem(&color, 0x10000);
   CPU_reg[1] = color;
   PGXP_WriteCB(&color, 0);
   PGXP_InvalidateRAMWord(0x10000);
   check(PGXP_GetColor(0, &word, rgb) && CPU_reg[1].flags == color.flags,
         "DMA preserves already consumed register and command copies");
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(!PGXP_GetColor(0, &word, rgb), "scattered DMA retires an actual word");
   WriteMem(&color, 0x10000);
   PGXP_InvalidateRAM(0x1f800000, UINT32_MAX, 0);
   PGXP_InvalidateRAM(0x1f801000, UINT32_MAX, 0);
   PGXP_InvalidateRAM(0x00800000, UINT32_MAX, 0);
   PGXP_InvalidateRAMWord(0x200000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   check(PGXP_GetColor(0, &word, rgb),
         "fresh owner recovers and invalid DMA addresses preserve RAM");
}

static void test_partial_words(void)
{
   PGXP_value memory, source, before;
   uint32_t word = UINT32_MAX;
   float rgb[3];
   unsigned mode, left, offset;
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   PGXP_pushRGBf(2047.9375f, 2047.9375f, 2047.9375f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   memory = *ReadMem(0x10000);
   PGXP_pushRGBf(300.25f, 300.25f, 300.25f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   source = *ReadMem(0x10000);
   for (mode = 0; mode < 2; mode++)
      for (left = 0; left < 2; left++)
         for (offset = 0; offset < 4; offset++)
         {
            int full = offset == (left ? 3 : 0);
            uint32_t instr = ((left ? 0x22u : 0x26u) << 26) | (1u << 16);
            PGXP_SetModes(PGXP_MODE_MEMORY | (mode ? PGXP_MODE_CPU : 0));
            WriteMem(&memory, 0x10000);
            CPU_reg[1] = source;
            if (left) PGXP_CPU_LWL(instr, word, 0x10000 + offset);
            else PGXP_CPU_LWR(instr, word, 0x10000 + offset);
            PGXP_WriteCB(&CPU_reg[1], 0);
            check(!!PGXP_GetColor(0, &word, rgb) == (full && mode),
                  "partial load refuses whole RGB, whole load preserves it");
            check(!memcmp(ReadMem(0x10000), &memory, sizeof(memory)),
                  "partial load preserves its unchanged memory owner");
            PGXP_CPU_LoadDelayed(instr, full ? word : 0x12345678, 0x10000 + offset);
            PGXP_CPU_CommitLoad(1, full ? word : 0x12345678, 32);
            check(!memcmp(ReadMem(0x10000), &memory, sizeof(memory)),
                  "delayed merged load does not validate source with merged bytes");
            if (!full)
               check(CPU_reg[1].value == 0x12345678 && CPU_reg[1].z == 0 &&
                     CPU_reg[1].flags == VALID_01,
                     "partial load keeps native coordinates without borrowed depth");
            CPU_reg[1] = source;
            PGXP_CPU_ValidateColor(&CPU_reg[1]);
            before = CPU_reg[1];
            if (left) PGXP_CPU_SWL(instr, word, 0x10000 + offset);
            else PGXP_CPU_SWR(instr, word, 0x10000 + offset);
            PGXP_WriteCB(ReadMem(0x10000), 0);
            check(!!PGXP_GetColor(0, &word, rgb) == (full && mode),
                  "partial store retires whole RGB, whole store preserves it");
            check(!memcmp(&before, &CPU_reg[1], sizeof(before)),
                  "partial store preserves its original register owner");
            if (full && mode)
               check(rgb[0] == 300.25f, "whole store uses register precision");
            WriteMem(&memory, 0x10000);
            PGXP_WriteCB(ReadMem(0x10000), 0);
            check(PGXP_GetColor(0, &word, rgb) && rgb[0] == 2047.9375f,
                  "fresh whole owner recovers after partial fallback");
         }
}

static void test_byte_store_retirement(void)
{
   static const uint32_t addresses[] = {0x10000, 0x80410000, 0x1f8003fc, 0xbf801810};
   PGXP_value original, expected;
   float rgb[3];
   uint32_t word = 0x30ff2010;
   unsigned i;
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   PGXP_pushRGBf(16.25f, 32.5f, 2047.9375f, word);
   PGXP_GTE_SWC2(itype(0, 22, 0), word, 0x10000);
   original = *ReadMem(0x10000);
   for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
   {
      WriteMem(&original, addresses[i]);
      expected = original;
      expected.flags = 0;
      expected.count = 7208;
      PGXP_CPU_SB(itype(0, 7, 0), (uint8_t)word, addresses[i]);
      check(!memcmp(&expected, ReadMem(addresses[i]), sizeof(expected)),
            "byte retirement changes only validity and diagnostic count");
      PGXP_WriteCB(ReadMem(addresses[i]), 0);
      check(!PGXP_GetColor(0, &word, rgb), "same-byte store retires RGB");
      SetValue(&CPU_reg[1], word);
      CPU_reg[1].x += 0.25f;
      PGXP_CPU_SH(itype(0, 1, 0), (uint16_t)word, addresses[i]);
      check(ReadMem(addresses[i])->value == word &&
            ReadMem(addresses[i])->x == 8208.25f &&
            ReadMem(addresses[i])->compFlags[0] == VALID &&
            ReadMem(addresses[i])->compFlags[1] == 0,
            "later halfword merge retains native halves and fresh precision");
      PGXP_CPU_LHU(itype(0, 2, 0), (uint16_t)word, addresses[i]);
      check(CPU_reg[2].x == 8208.25f && !(CPU_reg[2].flags & PGXP_COLOR),
            "fresh halfword precision reaches its actual CPU consumer");
   }
   WriteMem(&original, 0x10000);
   PGXP_CPU_SB(itype(0, 7, 0), 0, 0x00800000);
   check(!memcmp(&original, ReadMem(0x10000), sizeof(original)),
         "invalid byte-store address preserves unrelated owner");
}

int main(void)
{
#define RUN_TEST(fn) do { \
   unsigned before = failures, count = checks; \
   fn(); \
   printf("%s checks=%u failures=%u\n", #fn, checks - count, failures - before); \
} while (0)
   PGXP_Init();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   RUN_TEST(test_vertex_fallback);
   RUN_TEST(test_stored_depth);
   RUN_TEST(test_comparisons);
   RUN_TEST(test_hilo_moves);
   RUN_TEST(test_fractional_boundaries);
   RUN_TEST(test_halfword_extension);
   RUN_TEST(test_division);
   RUN_TEST(test_reset_and_modes);
   RUN_TEST(test_texture_modes);
   RUN_TEST(test_gte_alias);
   RUN_TEST(test_shifts);
   RUN_TEST(test_shift_sign_extension);
   RUN_TEST(test_zero_register);
   RUN_TEST(test_numeric_limits);
   RUN_TEST(test_memory_ownership);
   RUN_TEST(test_payload_domains);
   RUN_TEST(test_boolean_results);
   RUN_TEST(test_color_bitwise);
   RUN_TEST(test_memory_only_color);
   RUN_TEST(test_dma_ownership);
   RUN_TEST(test_partial_words);
   RUN_TEST(test_byte_store_retirement);
   PGXP_Shutdown();
   printf("precision checks=%u failures=%u\n", checks, failures);
   return failures != 0;
}
