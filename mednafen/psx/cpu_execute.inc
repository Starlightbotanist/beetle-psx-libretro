/* Shared interpreter body; CPU_RUN_NAME and CPU_RUN_PGXP select the variant. */
static int32_t CPU_RUN_NAME(PS_CPU *self, int32_t timestamp_in)
{
   int32_t timestamp = timestamp_in;
#if defined(HAVE_LIGHTREC) && defined(LIGHTREC_DEBUG)
   uint32_t oldpc;
#endif
   uint32_t PC;
   uint32_t new_PC;
   uint32_t LDWhich;
   uint32_t LDValue;

   gte_ts_done += timestamp;
   muldiv_ts_done += timestamp;

   BACKING_TO_ACTIVE;

#if defined(HAVE_LIGHTREC) && defined(LIGHTREC_DEBUG)
 oldpc = PC;
#endif

 do
 {
  while(MDFN_LIKELY(timestamp < next_event_ts))
  {
   uint32_t instr;
   uint32_t opf;

   /* The comparison above is the interpreter's event check point. */
   cpu_last_check_ts = timestamp;

   /* Zero must be zero...until the Master Plan is enacted. */
   GPR[0] = 0;

   /* Instruction fetch */
   if(MDFN_UNLIKELY(PC & 0x3))
   {
    // This will block interrupt processing, but since we're going more for keeping broken homebrew/hacks from working
    // than super-duper-accurate pipeline emulation, it shouldn't be a problem.
    CP0.BADA = PC;
    new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, 0);
    goto OpDone;
   }

   instr = ReadInstruction(&timestamp, PC);


   //
   // Instruction decode
   //
   opf = instr & 0x3F;

   if(instr & (0x3F << 26))
    opf = 0x40 | (instr >> 26);

   opf |= IPCache;

   if(ReadAbsorb[ReadAbsorbWhich])
    ReadAbsorb[ReadAbsorbWhich]--;
   else
    timestamp++;

   #define DO_LDS() { s_cpu.GPR_full[LDWhich] = LDValue; ReadAbsorb[LDWhich] = LDAbsorb; ReadFudge = LDWhich; ReadAbsorbWhich |= LDWhich & 0x1F; LDWhich = 0x22; }
   #define BEGIN_OPF(name) { op_##name:

#if HAVE_COMPUTED_GOTO && PSX_DISPATCH_REPLICATE
   /* Replicated dispatch: instead of every handler funnelling through
    * one central `goto *op_goto_table[opf]` site (a single indirect-
    * branch BTB entry shared by every opcode transition, i.e. near-
    * guaranteed mispredicts), each handler ends by fetching, decoding
    * and dispatching the next instruction itself.  Each of the ~90
    * resulting indirect-jump sites gets its own predictor context
    * keyed to that opcode's actual successor distribution.
    *
    * REDISPATCH is textually the loop-head sequence: event-slice
    * check, r0 pinning, ADEL check (rare path re-enters through the
    * central OpDone tail), ReadInstruction, decode, IPCache fold,
    * ReadAbsorb tick, table jump.  op_goto_table is in scope at all
    * expansion sites because handlers live in the same block as
    * CGBEGIN.  The central while-head remains as the entry path and
    * as the target of the exception route, so semantics are
    * identical instruction-for-instruction. */
   #define REDISPATCH \
	if(MDFN_UNLIKELY(timestamp >= next_event_ts)) \
	 goto RunSliceExit; \
	cpu_last_check_ts = timestamp; \
	GPR[0] = 0; \
	if(MDFN_UNLIKELY(PC & 0x3)) \
	{ \
	 CP0.BADA = PC; \
	 new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, 0); \
	 goto OpDone; \
	} \
	instr = ReadInstruction(&timestamp, PC); \
	opf = instr & 0x3F; \
	if(instr & (0x3F << 26)) \
	 opf = 0x40 | (instr >> 26); \
	opf |= IPCache; \
	if(ReadAbsorb[ReadAbsorbWhich]) \
	 ReadAbsorb[ReadAbsorbWhich]--; \
	else \
	 timestamp++; \
	/* Anti-merge construction, refined empirically against GCC 13 \
	 * -O3 cross-jumping (which merges common SUFFIXES): \
	 *   - marker at the head of REDISPATCH: bodies merged, 15 \
	 *     indirect sites survive; \
	 *   - marker before `goto *table[opf]`: table-load+jmp tails \
	 *     still merged, 22 sites; \
	 *   - this form: the loaded target is tied through a \
	 *     zero-instruction asm with a unique immediate per \
	 *     expansion, so everything up to and including the load is \
	 *     RTL-distinct and the only remaining common tail is the \
	 *     bare 2-byte `jmp *reg` - merging that behind a 5-byte \
	 *     direct jmp is a size regression cross-jumping rejects. */ \
	{ \
	 const void *dispatch_target_ = op_goto_table[opf]; \
	 __asm__ __volatile__ ("" : "+r" (dispatch_target_) : "i" (__COUNTER__)); \
	 goto *dispatch_target_; \
	}
   #define END_OPF { PC = new_PC; new_PC = new_PC + 4; BDBT = 0; REDISPATCH } }
   /* Branch handlers already set PC/new_PC/BDBT; they skip the
    * OpDone tail, matching the old `goto SkipNPCStuff`. */
   #define BRANCH_DISPATCH REDISPATCH
#else
   #define END_OPF goto OpDone; }
   #define BRANCH_DISPATCH goto SkipNPCStuff;
#endif

   #define DO_BRANCH(arg_cond, arg_offset, arg_mask, arg_dolink, arg_linkreg)\
	{							\
	 const bool cond = (arg_cond);				\
	 const uint32_t offset = (arg_offset);			\
	 const uint32_t mask = (arg_mask);			\
	 PC = new_PC;						\
	 new_PC += 4;						\
	 BDBT = 2;						\
								\
	 if(arg_dolink)						\
	  GPR[(arg_linkreg)] = new_PC;				\
								\
	 if(cond)						\
	 {							\
	  new_PC = ((new_PC - 4) & mask) + offset;		\
	  BDBT = 3;						\
	 }							\
								\
	 BRANCH_DISPATCH					\
	}

   #define ITYPE uint32_t rs MDFN_NOWARN_UNUSED = (instr >> 21) & 0x1F; uint32_t rt MDFN_NOWARN_UNUSED = (instr >> 16) & 0x1F; uint32_t immediate = (int32_t)(int16_t)(instr & 0xFFFF);
   #define ITYPE_ZE uint32_t rs MDFN_NOWARN_UNUSED = (instr >> 21) & 0x1F; uint32_t rt MDFN_NOWARN_UNUSED = (instr >> 16) & 0x1F; uint32_t immediate = instr & 0xFFFF;
   #define JTYPE uint32_t target = instr & ((1 << 26) - 1);
   #define RTYPE uint32_t rs MDFN_NOWARN_UNUSED = (instr >> 21) & 0x1F; uint32_t rt MDFN_NOWARN_UNUSED = (instr >> 16) & 0x1F; uint32_t rd MDFN_NOWARN_UNUSED = (instr >> 11) & 0x1F; uint32_t shamt MDFN_NOWARN_UNUSED = (instr >> 6) & 0x1F;

#if HAVE_COMPUTED_GOTO
   #if 0
	//
	// These truncated 32-bit table values apparently can't be calculated at compile/link time by gcc on x86_64, so gcc inserts initialization code, but
	// the compare for the initialization code path is placed sub-optimally(near where the table value is used instead of at the beginning of the function).
	//
	#define CGBEGIN static const uint32_t const op_goto_table[256] = {
	#define CGE(l) (uint32_t)(uintptr_t)&&l,
	#define CGEND }; goto *(void*)(uintptr_t)op_goto_table[opf];
   #else
	#define CGBEGIN static const void *const op_goto_table[256] = {
	#define CGE(l) &&l,
	#define CGEND }; goto *op_goto_table[opf];
   #endif
#else
   /* (uint8) cast for cheaper alternative to generated branch+compare bounds check instructions, but still more
      expensive than computed goto which needs no masking nor bounds checking.
   */
   #define CGBEGIN { enum { CGESB = 1 + __COUNTER__ }; switch((uint8_t)opf) {
   #define CGE(l) case __COUNTER__ - CGESB: goto l;
   #define CGEND } }
#endif

   CGBEGIN
    CGE(op_SLL)  CGE(op_ILL)   CGE(op_SRL)  CGE(op_SRA)   CGE(op_SLLV)    CGE(op_ILL)   CGE(op_SRLV) CGE(op_SRAV)
    CGE(op_JR)   CGE(op_JALR)  CGE(op_ILL)  CGE(op_ILL)   CGE(op_SYSCALL) CGE(op_BREAK) CGE(op_ILL)  CGE(op_ILL)
    CGE(op_MFHI) CGE(op_MTHI)  CGE(op_MFLO) CGE(op_MTLO)  CGE(op_ILL)     CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)
    CGE(op_MULT) CGE(op_MULTU) CGE(op_DIV)  CGE(op_DIVU)  CGE(op_ILL)     CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)
    CGE(op_ADD)  CGE(op_ADDU)  CGE(op_SUB)  CGE(op_SUBU)  CGE(op_AND)     CGE(op_OR)    CGE(op_XOR)  CGE(op_NOR)
    CGE(op_ILL)  CGE(op_ILL)   CGE(op_SLT)  CGE(op_SLTU)  CGE(op_ILL)     CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)
    CGE(op_ILL)  CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)   CGE(op_ILL)     CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)
    CGE(op_ILL)  CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)   CGE(op_ILL)     CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)

    CGE(op_ILL)  CGE(op_BCOND) CGE(op_J)    CGE(op_JAL)   CGE(op_BEQ)  CGE(op_BNE) CGE(op_BLEZ) CGE(op_BGTZ)
    CGE(op_ADDI) CGE(op_ADDIU) CGE(op_SLTI) CGE(op_SLTIU) CGE(op_ANDI) CGE(op_ORI) CGE(op_XORI) CGE(op_LUI)
    CGE(op_COP0) CGE(op_COP13) CGE(op_COP2) CGE(op_COP13) CGE(op_ILL)  CGE(op_ILL) CGE(op_ILL)  CGE(op_ILL)
    CGE(op_ILL)  CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL)   CGE(op_ILL)  CGE(op_ILL) CGE(op_ILL)  CGE(op_ILL)
    CGE(op_LB)   CGE(op_LH)    CGE(op_LWL)  CGE(op_LW)    CGE(op_LBU)  CGE(op_LHU) CGE(op_LWR)  CGE(op_ILL)
    CGE(op_SB)   CGE(op_SH)    CGE(op_SWL)  CGE(op_SW)    CGE(op_ILL)  CGE(op_ILL) CGE(op_SWR)  CGE(op_ILL)
    CGE(op_LWC013) CGE(op_LWC013)  CGE(op_LWC2) CGE(op_LWC013)  CGE(op_ILL)  CGE(op_ILL) CGE(op_ILL)  CGE(op_ILL)
    CGE(op_SWC013) CGE(op_SWC013)  CGE(op_SWC2) CGE(op_SWC013)  CGE(op_ILL)  CGE(op_ILL) CGE(op_ILL)  CGE(op_ILL)

    // Interrupt portion of this table is constructed so that an interrupt won't be taken when the PC is pointing to a GTE instruction,
    // to avoid problems caused by pipeline vs coprocessor nuances that aren't emulated.
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)

    CGE(op_ILL)       CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_COP2)      CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
    CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT) CGE(op_INTERRUPT)
   CGEND

   {
    BEGIN_OPF(ILL);
	     DO_LDS();
	     new_PC = CPU_Exception(EXCEPTION_RI, PC, new_PC, instr);
    END_OPF;

    //
    // ADD - Add Word
    //
    BEGIN_OPF(ADD);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] + GPR[rt];
	bool ep = ((~(GPR[rs] ^ GPR[rt])) & (GPR[rs] ^ result)) & 0x80000000;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_ADD(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	if(MDFN_UNLIKELY(ep))
	{
	 new_PC = CPU_Exception(EXCEPTION_OV, PC, new_PC, instr);
	}
	else
	 GPR[rd] = result;

    END_OPF;

    //
    // ADDI - Add Immediate Word
    //
    BEGIN_OPF(ADDI);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

        uint32_t result = GPR[rs] + immediate;
	bool ep = ((~(GPR[rs] ^ immediate)) & (GPR[rs] ^ result)) & 0x80000000;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_ADDI(instr, result, GPR[rs]);

	DO_LDS();

        if(MDFN_UNLIKELY(ep))
	{
	 new_PC = CPU_Exception(EXCEPTION_OV, PC, new_PC, instr);
	}
        else
         GPR[rt] = result;

    END_OPF;

    //
    // ADDIU - Add Immediate Unsigned Word
    //
    BEGIN_OPF(ADDIU);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] + immediate;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_ADDIU(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rt] = result;

    END_OPF;

    //
    // ADDU - Add Unsigned Word
    //
    BEGIN_OPF(ADDU);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] + GPR[rt];

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_ADDU(instr, result, GPR[rs], GPR[rt]);
	else if ((CPU_RUN_PGXP & PGXP_MODE_MEMORY) && rt == 0)
		PGXP_CPU_ADDU_Identity(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;

    //
    // AND - And
    //
    BEGIN_OPF(AND);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] & GPR[rt];

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_AND(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;

    //
    // ANDI - And Immediate
    //
    BEGIN_OPF(ANDI);
	ITYPE_ZE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] & immediate;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_ANDI(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rt] = result;

    END_OPF;

    //
    // BEQ - Branch on Equal
    //
    BEGIN_OPF(BEQ);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

	const bool result = (GPR[rs] == GPR[rt]);

	DO_LDS();

	DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
    END_OPF;

    // Bah, why does MIPS encoding have to be funky like this. :(
    // Handles BGEZ, BGEZAL, BLTZ, BLTZAL
    BEGIN_OPF(BCOND);
	const uint32_t tv = GPR[(instr >> 21) & 0x1F];
	const uint32_t riv = (instr >> 16) & 0x1F;
	const uint32_t immediate = (int32_t)(int16_t)(instr & 0xFFFF);
	const bool result = (int32_t)(tv ^ (riv << 31)) < 0;
	const uint32_t link = ((riv & 0x1E) == 0x10) ? 31 : 0;

	GPR_DEPRES_BEGIN
	GPR_DEP((instr >> 21) & 0x1F);
	GPR_RES(link);
	GPR_DEPRES_END

	DO_LDS();

	DO_BRANCH(result, (immediate << 2), ~0U, true, link);
    END_OPF;


    //
    // BGTZ - Branch on Greater than Zero
    //
    BEGIN_OPF(BGTZ);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

	const bool result = (int32_t)GPR[rs] > 0;

	DO_LDS();

	DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
    END_OPF;

    //
    // BLEZ - Branch on Less Than or Equal to Zero
    //
    BEGIN_OPF(BLEZ);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

	const bool result = (int32_t)GPR[rs] <= 0;

	DO_LDS();

	DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
    END_OPF;

    //
    // BNE - Branch on Not Equal
    //
    BEGIN_OPF(BNE);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

	const bool result = GPR[rs] != GPR[rt];

	DO_LDS();

	DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
    END_OPF;

    //
    // BREAK - Breakpoint
    //
    BEGIN_OPF(BREAK);
	DO_LDS();
	new_PC = CPU_Exception(EXCEPTION_BP, PC, new_PC, instr);
    END_OPF;

    // Cop "instructions":	CFCz(no CP0), COPz, CTCz(no CP0), LWCz(no CP0), MFCz, MTCz, SWCz(no CP0)
    //
    // COP0 instructions
    //
    BEGIN_OPF(COP0);
	const uint32_t sub_op = (instr >> 21) & 0x1F;
	const uint32_t rt = (instr >> 16) & 0x1F;
	const uint32_t rd = (instr >> 11) & 0x1F;
	const uint32_t val = GPR[rt];

	switch(sub_op)
	{
	 default:
		DO_LDS();
		break;

	 case 0x02:
	 case 0x06:
		DO_LDS();
		new_PC = CPU_Exception(EXCEPTION_RI, PC, new_PC, instr);
		break;

	 case 0x00:		// MFC0	- Move from Coprocessor
		switch(rd)
		{
		 case 0x00:
		 case 0x01:
		 case 0x02:
		 case 0x04:
		 case 0x0A:
			DO_LDS();
                        new_PC = CPU_Exception(EXCEPTION_RI, PC, new_PC, instr);
			break;

		 case 0x03:
		 case 0x05:
		 case 0x06:
		 case 0x07:
		 case 0x08:
		 case 0x09:
		 case 0x0B:
		 case 0x0C:
		 case 0x0D:
		 case 0x0E:
		 case 0x0F:
			if(MDFN_UNLIKELY(LDWhich == rt))
                         LDWhich = 0;

			DO_LDS();

			LDAbsorb = 0;
			LDWhich = rt;
			LDValue = cpu_CP0.Regs[rd];
			break;

		 default:
			// Tested to be rather NOPish
			DO_LDS();
			break;
		}
		break;

	 case 0x04:		// MTC0	- Move to Coprocessor
		DO_LDS();
		switch(rd)
		{
		 case 0x00:
		 case 0x01:
		 case 0x02:
		 case 0x04:
		 case 0x0A:
			new_PC = CPU_Exception(EXCEPTION_RI, PC, new_PC, instr);
			break;

		 case CP0REG_BPC:
			CP0.BPC = val;
			break;

		 case CP0REG_BDA:
			CP0.BDA = val;
			break;

		 case CP0REG_DCIC:
			CP0.DCIC = val & 0xFF80003F;
			break;

                 case CP0REG_BDAM:
			CP0.BDAM = val;
			break;

                 case CP0REG_BPCM:
			CP0.BPCM = val;
			break;

		 case CP0REG_CAUSE:
			CP0.CAUSE &= ~(0x3 << 8);
			CP0.CAUSE |= val & (0x3 << 8);
			CPU_RecalcIPCache();
			break;

		 case CP0REG_SR:
			CP0.SR = val & ~( (0x3 << 26) | (0x3 << 23) | (0x3 << 6));
			CPU_RecalcIPCache();
			break;
		}
		break;

	 case 0x08:	// BC
	 case 0x0C:
		DO_LDS();
		{
		 const uint32_t immediate = (int32_t)(int16_t)(instr & 0xFFFF);
		 const bool result = (false == (bool)(instr & (1U << 16)));
		 DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
		}
		break;

	 case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	 case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		DO_LDS();
		{
		 const uint32_t cp0_op = instr & 0x1F;	// Not 0x3F

		 if(MDFN_LIKELY(cp0_op == 0x10))	// RFE
		 {
		  // "Pop"
		  CP0.SR = (CP0.SR & ~0x0F) | ((CP0.SR >> 2) & 0x0F);
		  CPU_RecalcIPCache();
		 }
		 else if(cp0_op == 0x01 || cp0_op == 0x02 || cp0_op == 0x06 || cp0_op == 0x08)	// TLBR, TLBWI, TLBWR, TLBP
		 {
		  new_PC = CPU_Exception(EXCEPTION_RI, PC, new_PC, instr);
		 }
		}
		break;
	}
    END_OPF;

    //
    // COP2
    //
    BEGIN_OPF(COP2);
	const uint32_t sub_op = (instr >> 21) & 0x1F;
	const uint32_t rt = (instr >> 16) & 0x1F;
	const uint32_t rd = (instr >> 11) & 0x1F;
	const uint32_t val = GPR[rt];

	if(MDFN_UNLIKELY(!(CP0.SR & (1U << (28 + 2)))))
	{
	 DO_LDS();
         new_PC = CPU_Exception(EXCEPTION_COPU, PC, new_PC, instr);
	}
	else switch(sub_op)
	{
	 default:
		DO_LDS();
		break;

	 case 0x00:		// MFC2	- Move from Coprocessor
		if(MDFN_UNLIKELY(LDWhich == rt))
		 LDWhich = 0;

		DO_LDS();

	        if(timestamp < gte_ts_done)
		{
		 LDAbsorb = gte_ts_done - timestamp;
	         timestamp = gte_ts_done;
		}
		else
		 LDAbsorb = 0;

		LDWhich = rt;
		LDValue = GTE_ReadDR(rd);

                if (CPU_RUN_PGXP & PGXP_MODE_GTE)
                   PGXP_GTE_MFC2(instr, LDValue, LDValue);

		break;

	 case 0x04:		// MTC2	- Move to Coprocessor
		DO_LDS();

	        if(timestamp < gte_ts_done)
	         timestamp = gte_ts_done;

		GTE_WriteDR(rd, val);

                if (CPU_RUN_PGXP & PGXP_MODE_GTE)
                   PGXP_GTE_MTC2(instr, val, val);

		break;

	 case 0x02:		// CFC2
		if(MDFN_UNLIKELY(LDWhich == rt))
		 LDWhich = 0;

		DO_LDS();

	        if(timestamp < gte_ts_done)
		{
		 LDAbsorb = gte_ts_done - timestamp;
	         timestamp = gte_ts_done;
		}
		else
		 LDAbsorb = 0;

		LDWhich = rt;
		LDValue = GTE_ReadCR(rd);

                if (CPU_RUN_PGXP & PGXP_MODE_GTE)
                   PGXP_GTE_CFC2(instr, LDValue, LDValue);

		break;

	 case 0x06:		// CTC2
		DO_LDS();

                if(timestamp < gte_ts_done)
	         timestamp = gte_ts_done;

		GTE_WriteCR(rd, val);

                if (CPU_RUN_PGXP & PGXP_MODE_GTE)
                   PGXP_GTE_CTC2(instr, val, val);

		break;

	 case 0x08:
	 case 0x0C:
		DO_LDS();
		{
		 const uint32_t immediate = (int32_t)(int16_t)(instr & 0xFFFF);
		 const bool result = (false == (bool)(instr & (1U << 16)));
		 DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
		}
		break;

	 case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	 case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		DO_LDS();

	        if(timestamp < gte_ts_done)
	         timestamp = gte_ts_done;
		gte_ts_done = timestamp + GTE_Instruction(instr);
		break;
	}
    END_OPF;

    //
    // COP1, COP3
    //
    BEGIN_OPF(COP13);
	DO_LDS();

	if(!(CP0.SR & (1U << (28 + ((instr >> 26) & 0x3)))))
	{
         new_PC = CPU_Exception(EXCEPTION_COPU, PC, new_PC, instr);
	}
	else
	{
	 const uint32_t sub_op = (instr >> 21) & 0x1F;
	 if(sub_op == 0x08 || sub_op == 0x0C)
	 {
	  const uint32_t immediate = (int32_t)(int16_t)(instr & 0xFFFF);
	  const bool result = (false == (bool)(instr & (1U << 16)));

	  DO_BRANCH(result, (immediate << 2), ~0U, false, 0);
	 }
	}
    END_OPF;

    //
    // LWC0, LWC1, LWC3
    //
    BEGIN_OPF(LWC013);
        ITYPE;
	const uint32_t address = GPR[rs] + immediate;

	DO_LDS();

	if(!(CP0.SR & (1U << (28 + ((instr >> 26) & 0x3)))))
	{
         new_PC = CPU_Exception(EXCEPTION_COPU, PC, new_PC, instr);
	}
	else
	{
	 if(MDFN_UNLIKELY(address & 3))
	 {
	  CP0.BADA = address;
          new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, instr);
	 }
         else
          ReadMemory_u32(&timestamp, address, false, true);
	}
    END_OPF;

    //
    // LWC2
    //
    BEGIN_OPF(LWC2);
        ITYPE;
	const uint32_t address = GPR[rs] + immediate;

	DO_LDS();

        if(MDFN_UNLIKELY(address & 3))
	{
	 CP0.BADA = address;
         new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, instr);
	}
        else
	{
         if(timestamp < gte_ts_done)
          timestamp = gte_ts_done;

         uint32_t value = ReadMemory_u32(&timestamp, address, false, true);
         GTE_WriteDR(rt, value);

         if (CPU_RUN_PGXP & PGXP_MODE_GTE)
            PGXP_GTE_LWC2(instr, value, address);

	}
	// GTE stuff here
    END_OPF;

    //
    // SWC0, SWC1, SCW3
    //
    BEGIN_OPF(SWC013);
        ITYPE;
	const uint32_t address = GPR[rs] + immediate;

	DO_LDS();

	if(!(CP0.SR & (1U << (28 + ((instr >> 26) & 0x3)))))
	{
         new_PC = CPU_Exception(EXCEPTION_COPU, PC, new_PC, instr);
	}
	else
	{
	 if(MDFN_UNLIKELY(address & 0x3))
	 {
	  CP0.BADA = address;
	  new_PC = CPU_Exception(EXCEPTION_ADES, PC, new_PC, instr);
	 }
	}
    END_OPF;

    //
    // SWC2
    //
    BEGIN_OPF(SWC2);
        ITYPE;
	const uint32_t address = GPR[rs] + immediate;

	if(MDFN_UNLIKELY(address & 0x3))
	{
	 CP0.BADA = address;
	 new_PC = CPU_Exception(EXCEPTION_ADES, PC, new_PC, instr);
	}
	else
	{
         if(timestamp < gte_ts_done)
          timestamp = gte_ts_done;

	 WriteMemory_u32(&timestamp, address, GTE_ReadDR(rt), false);

	 if (CPU_RUN_PGXP & PGXP_MODE_GTE)
            PGXP_GTE_SWC2(instr, GTE_ReadDR(rt), address);
	}
	DO_LDS();
    END_OPF;

    //
    // DIV - Divide Word
    //
    BEGIN_OPF(DIV);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

        if(!GPR[rt])
        {
	 if(GPR[rs] & 0x80000000)
	  LO = 1;
	 else
	  LO = 0xFFFFFFFF;

	 HI = GPR[rs];
        }
	else if(GPR[rs] == 0x80000000 && GPR[rt] == 0xFFFFFFFF)
	{
	 LO = 0x80000000;
	 HI = 0;
	}
        else
        {
         LO = (int32_t)GPR[rs] / (int32_t)GPR[rt];
         HI = (int32_t)GPR[rs] % (int32_t)GPR[rt];
        }
	muldiv_ts_done = timestamp + 37;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_DIV(instr, HI, LO, GPR[rs], GPR[rt]);
	DO_LDS();

    END_OPF;


    //
    // DIVU - Divide Unsigned Word
    //
    BEGIN_OPF(DIVU);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

	if(!GPR[rt])
	{
	 LO = 0xFFFFFFFF;
	 HI = GPR[rs];
	}
	else
	{
	 LO = GPR[rs] / GPR[rt];
	 HI = GPR[rs] % GPR[rt];
	}
        muldiv_ts_done = timestamp + 37;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_DIVU(instr, HI, LO, GPR[rs], GPR[rt]);

	DO_LDS();
    END_OPF;

    //
    // J - Jump
    //
    BEGIN_OPF(J);
	JTYPE;

	DO_LDS();

	DO_BRANCH(true, target << 2, 0xF0000000, false, 0);
    END_OPF;

    //
    // JAL - Jump and Link
    //
    BEGIN_OPF(JAL);
	JTYPE;

	//GPR_DEPRES_BEGIN
	GPR_RES(31);
	//GPR_DEPRES_END

	DO_LDS();

	DO_BRANCH(true, target << 2, 0xF0000000, true, 31);
    END_OPF;

    //
    // JALR - Jump and Link Register
    //
    BEGIN_OPF(JALR);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t tmp = GPR[rs];

	DO_LDS();

	DO_BRANCH(true, tmp, 0, true, rd);
    END_OPF;

    //
    // JR - Jump Register
    //
    BEGIN_OPF(JR);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t bt = GPR[rs];

	DO_LDS();

	DO_BRANCH(true, bt, 0, false, 0);
    END_OPF;

    //
    // LUI - Load Upper Immediate
    //
    BEGIN_OPF(LUI);
	ITYPE_ZE;		// Actually, probably would be sign-extending...if we were emulating a 64-bit MIPS chip :b

	GPR_DEPRES_BEGIN
	GPR_RES(rt);
	GPR_DEPRES_END

	DO_LDS();

	GPR[rt] = immediate << 16;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_LUI(instr, GPR[rt]);

    END_OPF;

    //
    // MFHI - Move from HI
    //
    BEGIN_OPF(MFHI);
	RTYPE;

	GPR_DEPRES_BEGIN
        GPR_RES(rd);
	GPR_DEPRES_END

	DO_LDS();

	if(timestamp < muldiv_ts_done)
	{
	 if(timestamp == muldiv_ts_done - 1)
	  muldiv_ts_done--;
	 else
	 {
	  do
	  {
	   if(ReadAbsorb[ReadAbsorbWhich])
	    ReadAbsorb[ReadAbsorbWhich]--;
	   timestamp++;
	  } while(timestamp < muldiv_ts_done);
	 }
	}

	GPR[rd] = HI;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_MFHI(instr, GPR[rd], HI);

    END_OPF;


    //
    // MFLO - Move from LO
    //
    BEGIN_OPF(MFLO);
	RTYPE;

	GPR_DEPRES_BEGIN
        GPR_RES(rd);
	GPR_DEPRES_END

	DO_LDS();

	if(timestamp < muldiv_ts_done)
	{
	 if(timestamp == muldiv_ts_done - 1)
	  muldiv_ts_done--;
	 else
	 {
	  do
	  {
	   if(ReadAbsorb[ReadAbsorbWhich])
	    ReadAbsorb[ReadAbsorbWhich]--;
	   timestamp++;
	  } while(timestamp < muldiv_ts_done);
	 }
	}

	GPR[rd] = LO;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_MFLO(instr, GPR[rd], LO);

    END_OPF;


    //
    // MTHI - Move to HI
    //
    BEGIN_OPF(MTHI);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

	HI = GPR[rs];

	DO_LDS();

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_MTHI(instr, HI, GPR[rs]);

    END_OPF;

    //
    // MTLO - Move to LO
    //
    BEGIN_OPF(MTLO);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

	LO = GPR[rs];

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_MTLO(instr, LO, GPR[rs]);

	DO_LDS();

    END_OPF;


    //
    // MULT - Multiply Word
    //
    BEGIN_OPF(MULT);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

	uint64_t result;

	result = (int64_t)(int32_t)GPR[rs] * (int32_t)GPR[rt];
	muldiv_ts_done = timestamp + MULT_Tab24[MDFN_lzcount32((GPR[rs] ^ ((int32_t)GPR[rs] >> 31)) | 0x400)];
	DO_LDS();

	LO = result;
	HI = result >> 32;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_MULT(instr, HI, LO, GPR[rs], GPR[rt]);

    END_OPF;

    //
    // MULTU - Multiply Unsigned Word
    //
    BEGIN_OPF(MULTU);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

	uint64_t result;

	result = (uint64_t)GPR[rs] * GPR[rt];
	muldiv_ts_done = timestamp + MULT_Tab24[MDFN_lzcount32(GPR[rs] | 0x400)];
	DO_LDS();

	LO = result;
	HI = result >> 32;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_MULTU(instr, HI, LO, GPR[rs], GPR[rt]);

    END_OPF;


    //
    // NOR - NOR
    //
    BEGIN_OPF(NOR);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = ~(GPR[rs] | GPR[rt]);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_NOR(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;

    //
    // OR - OR
    //
    BEGIN_OPF(OR);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] | GPR[rt];

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_OR(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // ORI - OR Immediate
    //
    BEGIN_OPF(ORI);
	ITYPE_ZE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] | immediate;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_ORI(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rt] = result;

    END_OPF;


    //
    // SLL - Shift Word Left Logical
    //
    BEGIN_OPF(SLL);	// SLL
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rt] << shamt;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SLL(instr, result, GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SLLV - Shift Word Left Logical Variable
    //
    BEGIN_OPF(SLLV);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rt] << (GPR[rs] & 0x1F);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SLLV(instr, result, GPR[rt], GPR[rs]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;

    //
    // SLT - Set on Less Than
    //
    BEGIN_OPF(SLT);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = (bool)((int32_t)GPR[rs] < (int32_t)GPR[rt]);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SLT(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SLTI - Set on Less Than Immediate
    //
    BEGIN_OPF(SLTI);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

	uint32_t result = (bool)((int32_t)GPR[rs] < (int32_t)immediate);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SLTI(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rt] = result;

    END_OPF;


    //
    // SLTIU - Set on Less Than Immediate, Unsigned
    //
    BEGIN_OPF(SLTIU);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

	uint32_t result = (bool)(GPR[rs] < (uint32_t)immediate);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SLTIU(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rt] = result;

    END_OPF;


    //
    // SLTU - Set on Less Than, Unsigned
    //
    BEGIN_OPF(SLTU);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = (bool)(GPR[rs] < GPR[rt]);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SLTU(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SRA - Shift Word Right Arithmetic
    //
    BEGIN_OPF(SRA);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = ((int32_t)GPR[rt]) >> shamt;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SRA(instr, result, GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SRAV - Shift Word Right Arithmetic Variable
    //
    BEGIN_OPF(SRAV);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = ((int32_t)GPR[rt]) >> (GPR[rs] & 0x1F);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SRAV(instr, result, GPR[rt], GPR[rs]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SRL - Shift Word Right Logical
    //
    BEGIN_OPF(SRL);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rt] >> shamt;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SRL(instr, result, GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;

    //
    // SRLV - Shift Word Right Logical Variable
    //
    BEGIN_OPF(SRLV);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rt] >> (GPR[rs] & 0x1F);

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SRLV(instr, result, GPR[rt], GPR[rs]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SUB - Subtract Word
    //
    BEGIN_OPF(SUB);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] - GPR[rt];
	bool ep = (((GPR[rs] ^ GPR[rt])) & (GPR[rs] ^ result)) & 0x80000000;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SUB(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	if(MDFN_UNLIKELY(ep))
	{
	 new_PC = CPU_Exception(EXCEPTION_OV, PC, new_PC, instr);
	}
	else
	 GPR[rd] = result;

    END_OPF;


    //
    // SUBU - Subtract Unsigned Word
    //
    BEGIN_OPF(SUBU);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] - GPR[rt];

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_SUBU(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;


    //
    // SYSCALL
    //
    BEGIN_OPF(SYSCALL);
	DO_LDS();

	new_PC = CPU_Exception(EXCEPTION_SYSCALL, PC, new_PC, instr);
    END_OPF;


    //
    // XOR
    //
    BEGIN_OPF(XOR);
	RTYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
        GPR_RES(rd);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] ^ GPR[rt];

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_XOR(instr, result, GPR[rs], GPR[rt]);

	DO_LDS();

	GPR[rd] = result;

    END_OPF;

    //
    // XORI - Exclusive OR Immediate
    //
    BEGIN_OPF(XORI);
	ITYPE_ZE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_RES(rt);
	GPR_DEPRES_END

	uint32_t result = GPR[rs] ^ immediate;

	if (CPU_RUN_PGXP & PGXP_MODE_CPU)
		PGXP_CPU_XORI(instr, result, GPR[rs]);

	DO_LDS();

	GPR[rt] = result;
    END_OPF;

    //
    // Memory access instructions(besides the coprocessor ones) follow:
    //

    //
    // LB - Load Byte
    //
    BEGIN_OPF(LB);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

	uint32_t address = GPR[rs] + immediate;

	if(MDFN_UNLIKELY(LDWhich == rt))
	 LDWhich = 0;

	DO_LDS();

	LDWhich = rt;
	LDValue = (int32_t)(int8_t)ReadMemory_u8(&timestamp, address);

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_LB(instr, LDValue, address);
    END_OPF;

    //
    // LBU - Load Byte Unsigned
    //
    BEGIN_OPF(LBU);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

	if(MDFN_UNLIKELY(LDWhich == rt))
	 LDWhich = 0;

	DO_LDS();

        LDWhich = rt;
	LDValue = ReadMemory_u8(&timestamp, address);

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_LBU(instr, LDValue, address);
    END_OPF;

    //
    // LH - Load Halfword
    //
    BEGIN_OPF(LH);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

	if(MDFN_UNLIKELY(address & 1))
	{
	 DO_LDS();

	 CP0.BADA = address;
	 new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, instr);
	}
	else
	{
	 if(MDFN_UNLIKELY(LDWhich == rt))
	  LDWhich = 0;

	 DO_LDS();

	 LDWhich = rt;
         LDValue = (int32_t)(int16_t)ReadMemory_u16(&timestamp, address);
	}
	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_LH(instr, LDValue, address);
    END_OPF;

    //
    // LHU - Load Halfword Unsigned
    //
    BEGIN_OPF(LHU);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

        if(MDFN_UNLIKELY(address & 1))
	{
	 DO_LDS();

	 CP0.BADA = address;
         new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, instr);
	}
	else
	{
	 if(MDFN_UNLIKELY(LDWhich == rt))
	  LDWhich = 0;

	 DO_LDS();

	 LDWhich = rt;
         LDValue = ReadMemory_u16(&timestamp, address);
	}

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_LHU(instr, LDValue, address);
    END_OPF;


    //
    // LW - Load Word
    //
    BEGIN_OPF(LW);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

        if(MDFN_UNLIKELY(address & 3))
	{
	 DO_LDS();

	 CP0.BADA = address;
         new_PC = CPU_Exception(EXCEPTION_ADEL, PC, new_PC, instr);
	}
        else
	{
	 if(MDFN_UNLIKELY(LDWhich == rt))
	  LDWhich = 0;

	 DO_LDS();

	 LDWhich = rt;
         LDValue = ReadMemory_u32(&timestamp, address, false, false);
	}

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_LW(instr, LDValue, address);
    END_OPF;

    //
    // SB - Store Byte
    //
    BEGIN_OPF(SB);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

	uint32_t address = GPR[rs] + immediate;

	WriteMemory_u8(&timestamp, address, GPR[rt]);

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_SB(instr, GPR[rt], address);

	DO_LDS();
    END_OPF;

    //
    // SH - Store Halfword
    //
    BEGIN_OPF(SH);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

	if(MDFN_UNLIKELY(address & 0x1))
	{
	 CP0.BADA = address;
	 new_PC = CPU_Exception(EXCEPTION_ADES, PC, new_PC, instr);
	}
	else
	 WriteMemory_u16(&timestamp, address, GPR[rt]);

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_SH(instr, GPR[rt], address);

	DO_LDS();
    END_OPF;

    //
    // SW - Store Word
    //
    BEGIN_OPF(SW);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

	if(MDFN_UNLIKELY(address & 0x3))
	{
	 CP0.BADA = address;
	 new_PC = CPU_Exception(EXCEPTION_ADES, PC, new_PC, instr);
	}
	else
	 WriteMemory_u32(&timestamp, address, GPR[rt], false);

	if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_SW(instr, GPR[rt], address);

	DO_LDS();
    END_OPF;

    // LWL and LWR load delay slot tomfoolery appears to apply even to MFC0! (and probably MFCn and CFCn as well, though they weren't explicitly tested)

    //
    // LWL - Load Word Left
    //
    BEGIN_OPF(LWL);
	ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	//GPR_DEP(rt);
	GPR_DEPRES_END

	uint32_t address = GPR[rs] + immediate;
	uint32_t v = GPR[rt];

	if(LDWhich == rt)
	{
	 v = LDValue;
	 ReadFudge = 0;
	}
	else
	{
	 DO_LDS();
	}

	LDWhich = rt;
	switch(address & 0x3)
	{
	 case 0: LDValue = (v & ~(0xFF << 24)) | (ReadMemory_u8(&timestamp, address & ~3) << 24);
		 break;

	 case 1: LDValue = (v & ~(0xFFFF << 16)) | (ReadMemory_u16(&timestamp, address & ~3) << 16);
	         break;

	 case 2: LDValue = (v & ~(0xFFFFFF << 8)) | (ReadMemory_u32(&timestamp, address & ~3, true, false) << 8);
		 break;

	 case 3: LDValue = (v & ~(0xFFFFFFFF << 0)) | (ReadMemory_u32(&timestamp, address & ~3, false, false) << 0);
		 break;
	}

        if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
	   PGXP_CPU_LWL(instr, LDValue, address);

    END_OPF;

    //
    // SWL - Store Word Left
    //
    BEGIN_OPF(SWL);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

	switch(address & 0x3)
	{
	 case 0: WriteMemory_u8(&timestamp, address & ~3, GPR[rt] >> 24);
		 break;

	 case 1: WriteMemory_u16(&timestamp, address & ~3, GPR[rt] >> 16);
	         break;

	 case 2: WriteMemory_u32(&timestamp, address & ~3, GPR[rt] >> 8, true);
		 break;

	 case 3: WriteMemory_u32(&timestamp, address & ~3, GPR[rt] >> 0, false);
		 break;
	}
        if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
	   PGXP_CPU_SWL(instr, GPR[rt], address);

	DO_LDS();

    END_OPF;

    //
    // LWR - Load Word Right
    //
    BEGIN_OPF(LWR);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	//GPR_DEP(rt);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;
	uint32_t v = GPR[rt];

	if(LDWhich == rt)
	{
	 v = LDValue;
	 ReadFudge = 0;
	}
	else
	{
	 DO_LDS();
	}

	LDWhich = rt;
	switch(address & 0x3)
	{
	 case 0: LDValue = (v & ~(0xFFFFFFFF)) | ReadMemory_u32(&timestamp, address, false, false);
		 break;

	 case 1: LDValue = (v & ~(0xFFFFFF)) | ReadMemory_u32(&timestamp, address, true, false);
		 break;

	 case 2: LDValue = (v & ~(0xFFFF)) | ReadMemory_u16(&timestamp, address);
	         break;

	 case 3: LDValue = (v & ~(0xFF)) | ReadMemory_u8(&timestamp, address);
		 break;
	}

        if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
	   PGXP_CPU_LWR(instr, LDValue, address);

    END_OPF;

    //
    // SWR - Store Word Right
    //
    BEGIN_OPF(SWR);
        ITYPE;

	GPR_DEPRES_BEGIN
	GPR_DEP(rs);
	GPR_DEP(rt);
	GPR_DEPRES_END

        uint32_t address = GPR[rs] + immediate;

	switch(address & 0x3)
	{
	 case 0: WriteMemory_u32(&timestamp, address, GPR[rt], false);
		 break;

	 case 1: WriteMemory_u32(&timestamp, address, GPR[rt], true);
		 break;

	 case 2: WriteMemory_u16(&timestamp, address, GPR[rt]);
	         break;

	 case 3: WriteMemory_u8(&timestamp, address, GPR[rt]);
		 break;
	}

        if (CPU_RUN_PGXP & PGXP_MODE_MEMORY)
		PGXP_CPU_SWR(instr, GPR[rt], address);


	DO_LDS();

    END_OPF;

    //
    // Mednafen special instruction
    //
    BEGIN_OPF(INTERRUPT);
	if(Halted)
	{
	 goto SkipNPCStuff;
	}
	else
	{
         DO_LDS();

	 new_PC = CPU_Exception(EXCEPTION_INT, PC, new_PC, instr);
	}
    END_OPF;
   }

   OpDone: ;
   PC = new_PC;
   new_PC = new_PC + 4;
   BDBT = 0;

   SkipNPCStuff:	;
  }
#if HAVE_COMPUTED_GOTO && PSX_DISPATCH_REPLICATE
  RunSliceExit:	;
#endif
#if defined(HAVE_LIGHTREC) && defined(LIGHTREC_DEBUG)
  if (timestamp >= 0 && PC != oldpc)
     print_for_big_ass_debugger(timestamp, PC);
#endif
 } while(MDFN_LIKELY(PSX_EventHandler(timestamp)));

 if(gte_ts_done > 0)
  gte_ts_done -= timestamp;

 if(muldiv_ts_done > 0)
  muldiv_ts_done -= timestamp;

 ACTIVE_TO_BACKING;

 return(timestamp);
}

#undef DO_LDS
#undef BEGIN_OPF
#undef END_OPF
#undef REDISPATCH
#undef BRANCH_DISPATCH
#undef DO_BRANCH
#undef ITYPE
#undef ITYPE_ZE
#undef JTYPE
#undef RTYPE
#undef CGBEGIN
#undef CGE
#undef CGEND
