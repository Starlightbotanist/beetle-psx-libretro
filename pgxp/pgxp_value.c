#include <math.h>
#include <string.h>

#include "pgxp_value.h"
#include "limits.h"

void SetValue(PGXP_value *pV, uint32_t psxV)
{
	psx_value psx;
	psx.d = psxV;

	pV->x = psx.sw.l;
	pV->y = psx.sw.h;
	pV->z = 0.f;
	pV->flags = VALID_01;
	pV->value = psx.d;
}

void MakeValid(PGXP_value *pV, uint32_t psxV)
{
	MakeValidIfNeeded(pV, psxV);
}

/* Validate and MaskValidate are now static inline in pgxp_value.h
 * (called 40+ times per PGXP-tagged CPU instruction; the cross-TU
 * function-call overhead used to dominate their actual work). */

uint32_t ValueToTolerance(PGXP_value *pV, uint32_t psxV, float tolerance)
{
	psx_value psx;
	uint32_t       retFlags = VALID_ALL;

	psx.d = psxV;

	if (fabs(pV->x - psx.sw.l) >= tolerance)
		retFlags = retFlags & (VALID_1 | VALID_2 | VALID_3);

	if (fabs(pV->y - psx.sw.h) >= tolerance)
		retFlags = retFlags & (VALID_0 | VALID_2 | VALID_3);

	return retFlags;
}

/* Hot fixed-point wrapping helpers live in pgxp_value.h. */

double fu16Trunc(double in)
{
	/* Currently unused but kept for API completeness.  Original
	 * was `(u32)(in * 65536)` then divide back, which is UB on
	 * negative inputs.  floor() gives the same truncation
	 * behaviour for non-negative input and stays defined for
	 * negative input. */
	return floor(in * 65536.0) / 65536.0;
}
