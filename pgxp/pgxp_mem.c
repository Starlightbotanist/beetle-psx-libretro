#include <string.h>

#include "pgxp_mem.h"
#include "pgxp_main.h"
#include "pgxp_cpu.h"
#include "pgxp_gte.h"
#include "pgxp_value.h"

/* The PGXP shadow of PSX memory.  Previously a single
 * `Mem[3 * 2048 * 1024 / 4]` array of PGXP_value (42 MB) carved into
 * three equal 524288-entry slots, of which only:
 *   - the 524288-entry User RAM slot was actually filled,
 *   - the second slot needed 256 entries (1 KB scratch / 4 bytes),
 *   - the third slot needed 2048 entries (8 KB hardware regs / 4 bytes).
 * That left ~27 MB of unused PGXP_value in the middle and tail of
 * the buffer.  Splitting into three right-sized arrays drops the
 * static BSS from 42 MB to about 14.4 MB without changing the
 * external (u32) contract of PGXP_ConvertAddress.  The offset
 * constants are kept so callers that print or compare with
 * InvalidAddress continue to work, but the offsets are now packed
 * back-to-back instead of on 524288-entry strides. */
#define USER_MEM_COUNT		PGXP_RAM_WORDS	/* 524288 entries: 2 MB / 4 */
#define SCRATCH_MEM_COUNT	(1024 / 4)		/* 256 entries: 1 KB / 4 */
#define REGISTER_MEM_COUNT	(0x2000 / 4)		/* 2048 entries: 8 KB / 4 */

static PGXP_value UserMem[USER_MEM_COUNT];
static PGXP_value ScratchMem[SCRATCH_MEM_COUNT];
static PGXP_value RegisterMem[REGISTER_MEM_COUNT];

#define PGXP_MEM_TOTAL (USER_MEM_COUNT + SCRATCH_MEM_COUNT + REGISTER_MEM_COUNT)

/* One bit per initialized shadow word. Reset and state restore retire all
 * entries by clearing this bitmap. Copy-only readers can synthesize zero;
 * pointer readers and halfword merges materialize a zero entry on first use.
 * Full-word writes initialize the entire entry without reading the old bytes. */
uint32_t PGXP_MemInitialized[(PGXP_MEM_TOTAL + 31) / 32];

#ifdef HAVE_LIGHTREC
/* Match Lightrec's temporary first64KB RAM window. Already consumed register
 * and command copies remain independent owners when RAM is restored. */
#define ISOLATED_RAM_WORDS (64 * 1024 / 4)
static PGXP_value isolated_ram[ISOLATED_RAM_WORDS];
static uint32_t isolated_initialized[ISOLATED_RAM_WORDS / 32];
int PGXP_IsolatedRAMValid;

void PGXP_CacheIsolation(int isolated)
{
	if (isolated)
	{
		PGXP_IsolatedRAMValid = 0;
		if (PGXP_GetModes() & (PGXP_MODE_MEMORY | PGXP_MODE_GTE))
		{
			memcpy(isolated_ram, UserMem, sizeof(isolated_ram));
			memcpy(isolated_initialized, PGXP_MemInitialized,
				sizeof(isolated_initialized));
			PGXP_IsolatedRAMValid = 1;
		}
	}
	else
	{
		if (PGXP_IsolatedRAMValid)
		{
			memcpy(UserMem, isolated_ram, sizeof(isolated_ram));
			memcpy(PGXP_MemInitialized, isolated_initialized,
				sizeof(isolated_initialized));
		}
		else
			/* Reset or a mode change retired the original timeline. */
			memset(PGXP_MemInitialized, 0, sizeof(isolated_initialized));
		PGXP_IsolatedRAMValid = 0;
	}
}

void PGXP_InvalidateIsolatedRAMWord(uint32_t addr)
{
	uint32_t word = addr >> 2;
	if (word < ISOLATED_RAM_WORDS)
		isolated_initialized[word >> 5] &= ~((uint32_t)1 << (word & 31));
}
#endif

const uint32_t UserMemOffset  = 0;
const uint32_t ScratchOffset  = USER_MEM_COUNT;
const uint32_t RegisterOffset = USER_MEM_COUNT + SCRATCH_MEM_COUNT;
const uint32_t InvalidAddress = USER_MEM_COUNT + SCRATCH_MEM_COUNT + REGISTER_MEM_COUNT;

static inline int MemBitTest(uint32_t paddr)
{
	return (PGXP_MemInitialized[paddr >> 5] >> (paddr & 31)) & 1;
}

static inline void MemBitSet(uint32_t paddr)
{
	PGXP_MemInitialized[paddr >> 5] |= (uint32_t)1 << (paddr & 31);
}

/* Resolve an already-converted (packed) address to its entry.
 * Caller guarantees paddr < InvalidAddress. */
static inline PGXP_value* PtrFromPacked(uint32_t paddr)
{
	PGXP_value *value;
	if (paddr < ScratchOffset)
		value = &UserMem[paddr - UserMemOffset];
	else if (paddr < RegisterOffset)
		value = &ScratchMem[paddr - ScratchOffset];
	else
		value = &RegisterMem[paddr - RegisterOffset];
	if (!MemBitTest(paddr))
	{
		memset(value, 0, sizeof(*value));
		MemBitSet(paddr);
	}
	return value;
}

void PGXP_InitMem()
{
	/* Untouched entries are logically zero and initialized on pointer access.
	 * State restore only clears this small bitmap, not the full shadow RAM. */
	memset(PGXP_MemInitialized, 0, sizeof(PGXP_MemInitialized));
#ifdef HAVE_LIGHTREC
	PGXP_IsolatedRAMValid = 0;
#endif
}

/* Retire exactly [first, end), preserving adjacent words in the bitmap. */
static void InvalidateBitmapRange(uint32_t *bitmap, uint32_t first, uint32_t end)
{
	uint32_t first_bit = UINT32_MAX << (first & 31);
	uint32_t last_bit = UINT32_MAX >> (31 - ((end - 1) & 31));
	uint32_t first_index = first >> 5;
	uint32_t last_index = (end - 1) >> 5;
	if (first_index == last_index)
		bitmap[first_index] &= ~(first_bit & last_bit);
	else
	{
		bitmap[first_index] &= ~first_bit;
		memset(&bitmap[first_index + 1], 0,
			(last_index - first_index - 1) * sizeof(uint32_t));
		bitmap[last_index] &= ~last_bit;
	}
}

static void InvalidateRange(uint32_t first, uint32_t end)
{
	InvalidateBitmapRange(PGXP_MemInitialized, first, end);
#ifdef HAVE_LIGHTREC
	/* External DMA must not revive an overwritten owner when the isolated
	 * window ends. CPU stores use the active window, not this DMA entry. */
	if (PGXP_IsolatedRAMValid && first < ISOLATED_RAM_WORDS)
		InvalidateBitmapRange(isolated_initialized, first,
			end < ISOLATED_RAM_WORDS ? end : ISOLATED_RAM_WORDS);
#endif
}

void PGXP_InvalidateRAM(uint32_t addr, uint32_t words, int decrement)
{
	uint32_t first = PGXP_ConvertAddress(addr);
	if (!words || first >= USER_MEM_COUNT)
		return;
	if (words >= USER_MEM_COUNT)
		InvalidateRange(0, USER_MEM_COUNT);
	else if (decrement)
	{
		if (words <= first + 1)
			InvalidateRange(first + 1 - words, first + 1);
		else
		{
			InvalidateRange(0, first + 1);
			InvalidateRange(USER_MEM_COUNT - (words - first - 1), USER_MEM_COUNT);
		}
	}
	else if (words <= USER_MEM_COUNT - first)
		InvalidateRange(first, first + words);
	else
	{
		InvalidateRange(first, USER_MEM_COUNT);
		InvalidateRange(0, words - (USER_MEM_COUNT - first));
	}
}

/*  Playstation Memory Map (from Playstation doc by Joshua Walker)
0x0000_0000-0x0000_ffff		Kernel (64K)
0x0001_0000-0x001f_ffff		User Memory (1.9 Meg)

0x1f00_0000-0x1f00_ffff		Parallel Port (64K)

0x1f80_0000-0x1f80_03ff		Scratch Pad (1024 bytes)

0x1f80_1000-0x1f80_2fff		Hardware Registers (8K)

0x1fc0_0000-0x1fc7_ffff		BIOS (512K)

0x8000_0000-0x801f_ffff		Kernel and User Memory Mirror (2 Meg) Cached
0x9fc0_0000-0x9fc7_ffff		BIOS Mirror (512K) Cached

0xa000_0000-0xa01f_ffff		Kernel and User Memory Mirror (2 Meg) Uncached
0xbfc0_0000-0xbfc7_ffff		BIOS Mirror (512K) Uncached
*/
void ValidateAddress(uint32_t addr)
{
	if ((addr >= 0x00000000) && (addr <= 0x007fffff)) {}	/* Kernel + User Memory x 8 */
	else if ((addr >= 0x1f000000) && (addr <= 0x1f00ffff)) {}	/* Parallel Port */
	else if ((addr >= 0x1f800000) && (addr <= 0x1f8003ff)) {}	/* Scratch Pad */
	else if ((addr >= 0x1f801000) && (addr <= 0x1f802fff)) {}	/* Hardware Registers */
	else if ((addr >= 0x1fc00000) && (addr <= 0x1fc7ffff)) {}	/* Bios */
	else if ((addr >= 0x80000000) && (addr <= 0x807fffff)) {}	/* Kernel + User Memory x 8 Cached mirror */
	else if ((addr >= 0x9fc00000) && (addr <= 0x9fc7ffff)) {}	/* Bios Cached Mirror */
	else if ((addr >= 0xa0000000) && (addr <= 0xa07fffff)) {}	/* Kernel + User Memory x 8 Uncached mirror */
	else if ((addr >= 0xbfc00000) && (addr <= 0xbfc7ffff)) {}	/* Bios Uncached Mirror */
	else if (addr == 0xfffe0130) {}								/* Used for cache flushing */
	else
	{
		/* Unrecognized address - debug breakpoint hook would go here */
	}

}

uint32_t PGXP_ConvertAddress(uint32_t addr)
{
	uint32_t paddr = addr;

	/* KSEG0 and KSEG1 alias the physical bus; other segments do not. */
	if ((paddr & 0xc0000000u) == 0x80000000u)
		paddr &= 0x1fffffffu;
	if (paddr < 0x00800000u)
		return UserMemOffset + ((paddr & 0x1fffffu) >> 2);
	if (paddr >= 0x1f800000u && paddr < 0x1f800400u)
		return ScratchOffset + ((paddr - 0x1f800000u) >> 2);
	if (paddr >= 0x1f801000u && paddr < 0x1f803000u)
		return RegisterOffset + ((paddr - 0x1f801000u) >> 2);
	return InvalidAddress;
}

PGXP_value* GetPtr(uint32_t addr)
{
	addr = PGXP_ConvertAddress(addr);

	if (addr < InvalidAddress)
		return PtrFromPacked(addr);
	return NULL;
}

PGXP_value* ReadMem(uint32_t addr)
{
	return GetPtr(addr);
}

void PGXP_InvalidateMem(uint32_t addr)
{
	uint32_t paddr = PGXP_ConvertAddress(addr);
	if (paddr < InvalidAddress)
		PGXP_MemInitialized[paddr >> 5] &= ~((uint32_t)1 << (paddr & 31));
}

void ValidateAndCopyMem(PGXP_value* dest, uint32_t addr, uint32_t value)
{
	uint32_t paddr = PGXP_ConvertAddress(addr);
	if (paddr < InvalidAddress)
	{
		PGXP_value* pMem;

		if (!MemBitTest(paddr))
		{
			/* Retired entries are logically zero. Synthesize that
			 * without reading stale shadow bytes or touching their
			 * cache line. */
			memset(dest, 0, sizeof(*dest));
			return;
		}

		pMem = PtrFromPacked(paddr);
		Validate(pMem, value);
		*dest = *pMem;
		return;
	}

	*dest = PGXP_value_invalid_address;
}

void ValidateAndCopyMem16(PGXP_value* dest, uint32_t addr, uint32_t value, int sign)
{
	uint32_t validMask = 0;
	psx_value val, mask;
	PGXP_value zero_local;
	PGXP_value* pMem = NULL;
	uint32_t paddr = PGXP_ConvertAddress(addr);

	if (paddr < InvalidAddress)
	{
		if (!MemBitTest(paddr))
		{
			/* Never-written entry is the memset zero; run the
			 * identical merge/truncate body below against a local
			 * zero so the result is bit-for-bit what reading the
			 * array would produce, without touching its cache
			 * line.  MaskValidate's writeback lands in the
			 * throwaway. */
			memset(&zero_local, 0, sizeof(zero_local));
			pMem = &zero_local;
		}
		else
			pMem = PtrFromPacked(paddr);
	}

	if (pMem != NULL)
	{
		mask.d = val.d = 0;
		/* determine if high or low word */
		if ((addr % 4) == 2)
		{
			val.w.h = value;
			mask.w.h = 0xFFFF;
			validMask = VALID_1;
		}
		else
		{
			val.w.l = value;
			mask.w.l = 0xFFFF;
			validMask = VALID_0;
		}

		/* validate and copy whole value */
		MaskValidate(pMem, val.d, mask.d, validMask);
		*dest = *pMem;

		/* if high word then shift */
		dest->flags &= ~PGXP_COLOR;
		if ((addr % 4) == 2)
		{
			dest->x = dest->y;
			dest->lFlags = dest->hFlags;
			dest->compFlags[0] = dest->compFlags[1];
		}

		/* truncate value */
		dest->y = (sign && (value & 0x8000u)) ? -1.f : 0.f;
		dest->hFlags = 0;
		dest->value = value;
		dest->compFlags[1] = VALID;	/* iCB: High word is valid, just 0 */
		return;
	}

	*dest = PGXP_value_invalid_address;
}

void WriteMem(PGXP_value* value, uint32_t addr)
{
	uint32_t paddr = PGXP_ConvertAddress(addr);

	if (paddr < InvalidAddress)
	{
		MemBitSet(paddr);
		*PtrFromPacked(paddr) = *value;
	}
}

void WriteMem16(PGXP_value* src, uint32_t addr)
{
	PGXP_value* dest = NULL;
	uint32_t paddr = PGXP_ConvertAddress(addr);

	if (paddr < InvalidAddress)
	{
		/* Materialize both halves before preserving the untouched half. */
		dest = PtrFromPacked(paddr);
	}

	if (dest)
	{
		/* Load dest->value into a local psx_value, manipulate, store
		 * back.  The previous code aliased dest->value (a u32) through
		 * a psx_value pointer, which is a strict-aliasing violation
		 * (u32 and psx_value are not compatible types in C's effective
		 * type rules).  The round-trip through a local is bit-exact,
		 * defined behaviour, and the compiler optimises it identically
		 * on every backend we ship. */
		psx_value pVal;
		pVal.d = dest->value;

		/* determine if high or low word */
		if ((addr % 4) == 2)
		{
			dest->y = src->x;
			dest->hFlags = src->lFlags;
			dest->compFlags[1] = src->compFlags[0];
			pVal.w.h = (uint16_t)src->value;
		}
		else
		{
			dest->x = src->x;
			dest->lFlags = src->lFlags;
			dest->compFlags[0] = src->compFlags[0];
			pVal.w.l = (uint16_t)src->value;
		}

		dest->value = pVal.d;
		dest->flags &= ~PGXP_COLOR;

		/* overwrite z/w if valid */
		if (src->compFlags[2] == VALID)
		{
			dest->z = src->z;
			dest->compFlags[2] = src->compFlags[2];
		}


		/*dest->valid = dest->valid && src->valid; */
		dest->gFlags |= src->gFlags;				/* inherit flags from both values (?) */
	}
}
