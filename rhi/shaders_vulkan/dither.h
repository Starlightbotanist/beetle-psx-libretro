#ifndef DITHER_H_
#define DITHER_H_

#include "color.h"

layout(set = 0, binding = 3, std140) uniform DitherInfo
{
	int dither_shift;
} dither_info;

highp vec3 apply_dither(highp vec3 color, ivec2 coord)
{
	ivec2 wrapped_coord = (coord >> dither_info.dither_shift) & 3; // Dither pattern repeats every four pixels.
	/* Preserve the legacy scanout approximation. Native 15-bit rendering
	 * quantizes per primitive with psx_color5 and bypasses this pass. */
	float biased_offset = psx_dither_offset(wrapped_coord) + 4.0;
	return floor((color + vec3(biased_offset / 255.0)) * 31.0) / 31.0;
}

#endif
