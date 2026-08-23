/* Final-OpenGL-stream PGXP short-edge repair checks.
 *
 * These cases exercise the real sidecar, topology search, projection and
 * conflict resolver without requiring a GL context.  The synthetic command
 * vertex keeps position first, which is the only renderer ABI the diagnostic
 * consumes. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../pgxp/pgxp_diag.h"
#include "../../pgxp/pgxp_main.h"

#ifndef PGXP_DIAG_GL_REPAIR_APPLY
#define PGXP_DIAG_GL_REPAIR_APPLY 0
#endif

typedef struct
{
	float position[4];
	uint32_t padding[4];
} test_vertex;

static int failures;
static uint64_t submit_material_key = 1;

static void submit_triangle_ex(test_vertex output[3],
		const int32_t native[3][2], const float precise[3][2],
		uint8_t opcode, int invalid_w)
{
	PGXP_diag_primitive_vertex submitted[3];
	unsigned i;
	memset(submitted, 0, sizeof(submitted));
	memset(output, 0, 3u * sizeof(output[0]));
	PGXP_DiagPacket(opcode, 7, 0, 0, 0);
	for (i = 0; i < 3; i++)
	{
		submitted[i].native_x = native[i][0];
		submitted[i].native_y = native[i][1];
		submitted[i].precise_before_x = precise[i][0];
		submitted[i].precise_before_y = precise[i][1];
		submitted[i].precise_before_w = 1.0f;
		submitted[i].precise_after_x = precise[i][0];
		submitted[i].precise_after_y = precise[i][1];
		submitted[i].precise_after_w = 1.0f;
		output[i].position[0] = precise[i][0];
		output[i].position[1] = precise[i][1];
		output[i].position[3] = 1.0f;
	}
	PGXP_DiagSubmitPrimitive(submitted, 3, invalid_w, 0);
	PGXP_DiagGLPrimitive(output, 3, (unsigned)sizeof(output[0]),
		submit_material_key);
}

static void submit_triangle(test_vertex output[3],
		const int32_t native[3][2], const float precise[3][2],
		uint8_t opcode)
{
	submit_triangle_ex(output, native, precise, opcode, 0);
}

static void expect_near(const char* name, float actual, float expected)
{
	if (fabsf(actual - expected) > 1.0e-6f)
	{
		fprintf(stderr, "FAIL %-38s got=%g want=%g\n",
			name, actual, expected);
		failures++;
	}
}

static void expect_mask(const char* name, unsigned actual, unsigned expected)
{
	if (actual != expected)
	{
		fprintf(stderr, "FAIL %-38s got=0x%x want=0x%x\n",
			name, actual, expected);
		failures++;
	}
}

static void test_opposing_tjunction_closes(void)
{
	static const int32_t long_native[3][2] = {
		{ 0, 0 }, { 10, 0 }, { 0, 10 }
	};
	static const float long_precise[3][2] = {
		{ 0.0f, 0.0f }, { 10.0f, 0.0f }, { 0.0f, 10.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 0 }, { 10, 0 }, { 5, -10 }
	};
	static const float short_precise[3][2] = {
		{ 5.0f, -0.5f }, { 10.0f, -0.5f }, { 5.0f, -10.0f }
	};
	test_vertex stream[6];

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_REPAIR_APPLY ?
		PGXP_DIAG_GL_TEST_BROAD_REPLAY : PGXP_DIAG_GL_TEST_OFF);
	printf("[R1] opposing short edge %s the long PGXP edge\n",
		PGXP_DIAG_GL_REPAIR_APPLY ? "projects onto" : "is modeled against");
	submit_triangle(&stream[0], long_native, long_precise, 0x2c);
	submit_triangle(&stream[3], short_native, short_precise, 0x2c);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_near("junction point x", stream[3].position[0], 5.0f);
	expect_near("junction point y", stream[3].position[1],
		PGXP_DIAG_GL_REPAIR_APPLY ? 0.0f : -0.5f);
	expect_near("linked endpoint x", stream[4].position[0], 10.0f);
	expect_near("linked endpoint y", stream[4].position[1],
		PGXP_DIAG_GL_REPAIR_APPLY ? 0.0f : -0.5f);
	expect_near("point triangle third y", stream[5].position[1], -10.0f);
	PGXP_DiagFrame(0); /* exercise the frame-complete link classifier */
}

static void test_conflicting_targets_are_atomic(void)
{
	static const int32_t long_native[3][2] = {
		{ 0, 0 }, { 10, 0 }, { 0, 10 }
	};
	static const float first_long[3][2] = {
		{ 0.0f, 0.0f }, { 10.0f, 0.0f }, { 0.0f, 10.0f }
	};
	static const float second_long[3][2] = {
		/* Same target as the first edge at the linked endpoint, but a
		 * conflicting target at the interior junction.  Atomic rejection must
		 * therefore propagate to the otherwise-consistent endpoint. */
		{ 0.0f, 1.0f }, { 10.0f, 0.0f }, { 0.0f, 11.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 0 }, { 10, 0 }, { 5, -10 }
	};
	static const float short_precise[3][2] = {
		{ 5.0f, -1.0f }, { 10.0f, -1.0f }, { 5.0f, -10.0f }
	};
	test_vertex stream[9];

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_BROAD_REPLAY);
	printf("[R2] disagreeing long-edge targets reject the whole short edge\n");
	submit_triangle(&stream[0], long_native, first_long, 0x2c);
	submit_triangle(&stream[3], long_native, second_long, 0x2c);
	submit_triangle(&stream[6], short_native, short_precise, 0x2c);
	PGXP_DiagGLRepair(stream, 9, (unsigned)sizeof(stream[0]));
	expect_near("conflicted junction point y", stream[6].position[1], -1.0f);
	expect_near("conflicted linked endpoint y", stream[7].position[1], -1.0f);
	PGXP_DiagFrame(0);
}

static void test_pgxp_off_is_untouched(void)
{
	static const int32_t long_native[3][2] = {
		{ 0, 0 }, { 10, 0 }, { 0, 10 }
	};
	static const float long_precise[3][2] = {
		{ 0.0f, 0.0f }, { 10.0f, 0.0f }, { 0.0f, 10.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 0 }, { 10, 0 }, { 5, -10 }
	};
	static const float short_precise[3][2] = {
		{ 5.0f, -0.5f }, { 10.0f, -0.5f }, { 5.0f, -10.0f }
	};
	test_vertex stream[6];

	printf("[R3] PGXP-off final stream is never modified\n");
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_BROAD_REPLAY);
	PGXP_SetModes(PGXP_MODE_NONE);
	submit_triangle(&stream[0], long_native, long_precise, 0x2c);
	submit_triangle(&stream[3], short_native, short_precise, 0x2c);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_near("PGXP-off junction point y", stream[3].position[1], -0.5f);
	expect_near("PGXP-off linked endpoint y", stream[4].position[1], -0.5f);
	PGXP_DiagFrame(0);
	PGXP_SetModes(PGXP_MODE_MEMORY);
}

static void run_endpoint_mode(unsigned mode, float short_y,
		float linked_y, test_vertex stream[6])
{
	static const int32_t long_native[3][2] = {
		{ 0, 20 }, { 10, 20 }, { 0, 30 }
	};
	static const float long_precise[3][2] = {
		{ 0.0f, 20.0f }, { 10.0f, 20.0f }, { 0.0f, 30.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 20 }, { 10, 20 }, { 5, 10 }
	};
	float short_precise[3][2] = {
		{ 5.0f, short_y }, { 10.0f, linked_y }, { 5.0f, 10.0f }
	};
	PGXP_DiagGLSetMode(mode);
	submit_triangle(&stream[0], long_native, long_precise, 0x2c);
	submit_triangle(&stream[3], short_native, short_precise, 0x2c);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
}

static void run_endpoint_material_mismatch(unsigned mode,
		test_vertex stream[6])
{
	static const int32_t long_native[3][2] = {
		{ 0, 20 }, { 10, 20 }, { 0, 30 }
	};
	static const float long_precise[3][2] = {
		{ 0.0f, 20.0f }, { 10.0f, 20.0f }, { 0.0f, 30.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 20 }, { 10, 20 }, { 5, 10 }
	};
	static const float short_precise[3][2] = {
		{ 5.0f, 19.25f }, { 10.0f, 19.25f }, { 5.0f, 10.0f }
	};
	PGXP_DiagGLSetMode(mode);
	submit_material_key = 1;
	submit_triangle(&stream[0], long_native, long_precise, 0x2c);
	submit_material_key = 2;
	submit_triangle(&stream[3], short_native, short_precise, 0x2c);
	submit_material_key = 1;
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
}

static void run_interior_mode(unsigned mode, test_vertex stream[6])
{
	static const int32_t long_native[3][2] = {
		{ 0, 20 }, { 10, 20 }, { 0, 30 }
	};
	static const float long_precise[3][2] = {
		{ 0.0f, 20.0f }, { 10.0f, 20.0f }, { 0.0f, 30.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 20 }, { 8, 20 }, { 5, 10 }
	};
	static const float short_precise[3][2] = {
		{ 5.0f, 19.25f }, { 8.0f, 19.25f }, { 5.0f, 10.0f }
	};
	PGXP_DiagGLSetMode(mode);
	submit_triangle(&stream[0], long_native, long_precise, 0x2c);
	submit_triangle(&stream[3], short_native, short_precise, 0x2c);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
}

static void run_native_handoff_mode(unsigned mode, uint8_t opcode,
		int invalid_w, float dx, float dy, test_vertex stream[3])
{
	static const int32_t native[3][2] = {
		{ 10, 20 }, { 20, 20 }, { 10, 30 }
	};
	float precise[3][2] = {
		{ 10.0f + dx, 20.0f + dy },
		{ 20.0f + dx, 20.0f + dy },
		{ 10.0f + dx, 30.0f + dy }
	};
	PGXP_DiagGLSetMode(mode);
	submit_triangle_ex(stream, native, precise, opcode, invalid_w);
	PGXP_DiagGLRepair(stream, 3, (unsigned)sizeof(stream[0]));
}

static void test_runtime_mode_matrix(void)
{
	test_vertex stream[6];
	unsigned mode;
	char label[96];

	puts("[R4] runtime seam-test modes obey coverage and link-kind gates");
	run_endpoint_mode(PGXP_DIAG_GL_TEST_OFF, 19.25f, 19.25f, stream);
	expect_near("off leaves endpoint point", stream[3].position[1], 19.25f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_NATIVE_BOUNDARY,
		19.25f, 19.25f, stream);
	expect_near("native control moves endpoint point", stream[3].position[1], 20.0f);
	expect_near("native control moves endpoint link", stream[4].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_NATIVE_T_IMPROVED,
		19.25f, 19.25f, stream);
	expect_near("native-t improved point", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED,
		19.25f, 19.25f, stream);
	expect_near("native-t closed point", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_ENDPOINT,
		19.25f, 19.25f, stream);
	expect_near("native-t endpoint accepts endpoint", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_INTERIOR,
		19.25f, 19.25f, stream);
	expect_near("native-t interior rejects endpoint", stream[3].position[1], 19.25f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_MATERIAL,
		19.25f, 19.25f, stream);
	expect_near("native-t material accepts match", stream[3].position[1], 20.0f);
	run_endpoint_material_mismatch(
		PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_MATERIAL, stream);
	expect_near("native-t material rejects mismatch", stream[3].position[1], 19.25f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_IMPROVED,
		19.25f, 19.25f, stream);
	expect_near("perpendicular improved point", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_CLOSED,
		19.25f, 19.25f, stream);
	expect_near("perpendicular closed point", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_CLOSED_ENDPOINT,
		19.25f, 19.25f, stream);
	expect_near("perpendicular endpoint accepts endpoint", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_CLOSED_INTERIOR,
		19.25f, 19.25f, stream);
	expect_near("perpendicular interior rejects endpoint", stream[3].position[1], 19.25f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_POINT_CLOSED,
		19.25f, 20.0f, stream);
	expect_near("perpendicular point-only closes junction", stream[3].position[1], 20.0f);
	expect_near("perpendicular point-only preserves link", stream[4].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_IMPROVED_MATERIAL,
		19.25f, 19.25f, stream);
	expect_near("perpendicular improved material match", stream[3].position[1], 20.0f);
	run_endpoint_mode(PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL,
		19.25f, 19.25f, stream);
	expect_near("perpendicular closed material match", stream[3].position[1], 20.0f);
	run_endpoint_material_mismatch(
		PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL, stream);
	expect_near("perpendicular material rejects mismatch", stream[3].position[1], 19.25f);
	run_interior_mode(PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_INTERIOR, stream);
	expect_near("native-t interior accepts interior", stream[3].position[1], 20.0f);
	run_interior_mode(PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_ENDPOINT, stream);
	expect_near("native-t endpoint rejects interior", stream[3].position[1], 19.25f);
	run_interior_mode(PGXP_DIAG_GL_TEST_PERP_CLOSED_INTERIOR, stream);
	expect_near("perpendicular interior accepts interior", stream[3].position[1], 20.0f);
	for (mode = PGXP_DIAG_GL_TEST_SWAN_Y_POS;
	     mode <= PGXP_DIAG_GL_TEST_UPPER_LEFT_NEAREST; mode++)
	{
		run_endpoint_mode(mode, 19.25f, 19.25f, stream);
		snprintf(label, sizeof(label),
			"raster mode %u leaves CPU point", mode);
		expect_near(label, stream[3].position[1], 19.25f);
		snprintf(label, sizeof(label),
			"raster mode %u leaves CPU link", mode);
		expect_near(label, stream[4].position[1], 19.25f);
		if (PGXP_DiagGLGetMode() != mode)
		{
			printf("  FAIL raster mode getter: got %u expected %u\n",
				PGXP_DiagGLGetMode(), mode);
			failures++;
		}
	}
	run_endpoint_mode(PGXP_DIAG_GL_TEST_VULKAN_CLIP_MATH,
		19.25f, 19.25f, stream);
	expect_near("Vulkan clip math leaves CPU point",
		stream[3].position[1], 19.25f);
	expect_near("Vulkan clip math leaves CPU link",
		stream[4].position[1], 19.25f);
	for (mode = PGXP_DIAG_GL_TEST_CONSERVATIVE_RASTER;
	     mode < PGXP_DIAG_GL_TEST_COUNT; mode++)
	{
		run_endpoint_mode(mode, 19.25f, 19.25f, stream);
		snprintf(label, sizeof(label),
			"renderer mode %u leaves CPU point", mode);
		expect_near(label, stream[3].position[1], 19.25f);
		snprintf(label, sizeof(label),
			"renderer mode %u leaves CPU link", mode);
		expect_near(label, stream[4].position[1], 19.25f);
		if (PGXP_DiagGLGetMode() != mode)
		{
			printf("  FAIL renderer mode getter: got %u expected %u\n",
				PGXP_DiagGLGetMode(), mode);
			failures++;
		}
	}

	puts("[R5] native opaque-textured modes partition the positive control");
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_ALL,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("native all x", stream[0].position[0], 10.0f);
	expect_near("native all y", stream[0].position[1], 20.0f);

	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_TRI,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("flat triangle selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_TRI,
		0x34, 0, 0.25f, -0.75f, stream);
	expect_near("flat triangle rejects gouraud", stream[0].position[1], 19.25f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_QUAD,
		0x2c, 0, 0.25f, -0.75f, stream);
	expect_near("flat quad selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_GOURAUD_TRI,
		0x34, 0, 0.25f, -0.75f, stream);
	expect_near("gouraud triangle selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_GOURAUD_QUAD,
		0x3c, 0, 0.25f, -0.75f, stream);
	expect_near("gouraud quad selected", stream[0].position[1], 20.0f);

	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_X_ONLY,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("native X selected", stream[0].position[0], 10.0f);
	expect_near("native X preserves Y", stream[0].position[1], 19.25f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_Y_ONLY,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("native Y preserves X", stream[0].position[0], 10.25f);
	expect_near("native Y selected", stream[0].position[1], 20.0f);

	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_SMALL,
		0x24, 0, 0.25f, -0.25f, stream);
	expect_near("small delta selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_SMALL,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("small delta rejects large", stream[0].position[1], 19.25f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_LARGE,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("large delta selected", stream[0].position[1], 20.0f);

	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_VALID_W,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("valid W selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_VALID_W,
		0x24, 1, 0.25f, -0.75f, stream);
	expect_near("valid W rejects invalid", stream[0].position[1], 19.25f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_NATIVE_OT_INVALID_W,
		0x24, 1, 0.25f, -0.75f, stream);
	expect_near("invalid W selected", stream[0].position[1], 20.0f);

	puts("[R6] continuity-preserving native-axis modes blend or gate per vertex");
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_BLEND_50,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("Y blend 50 preserves X", stream[0].position[0], 10.25f);
	expect_near("Y blend 50", stream[0].position[1], 19.625f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_BLEND_75,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("Y blend 75", stream[0].position[1], 19.8125f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_BLEND_875,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("Y blend 87.5", stream[0].position[1], 19.90625f);

	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_GT_HALF,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("Y > 0.5 selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_GT_HALF,
		0x24, 0, 0.25f, -0.25f, stream);
	expect_near("Y > 0.5 rejects small", stream[0].position[1], 19.75f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_GT_QUARTER,
		0x24, 0, 0.25f, -0.5f, stream);
	expect_near("Y > 0.25 selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_GT_QUARTER,
		0x24, 0, 0.25f, -0.25f, stream);
	expect_near("Y > 0.25 strict boundary", stream[0].position[1], 19.75f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_LE_HALF,
		0x24, 0, 0.25f, -0.5f, stream);
	expect_near("Y <= 0.5 selected", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_LE_HALF,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("Y <= 0.5 rejects large", stream[0].position[1], 19.25f);

	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_25,
		0x24, 0, 0.8f, -0.75f, stream);
	expect_near("native Y + X blend 25 X", stream[0].position[0], 10.6f);
	expect_near("native Y + X blend 25 Y", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_50,
		0x24, 0, 0.8f, -0.75f, stream);
	expect_near("native Y + X blend 50 X", stream[0].position[0], 10.4f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_75,
		0x24, 0, 0.8f, -0.75f, stream);
	expect_near("native Y + X blend 75 X", stream[0].position[0], 10.2f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_GT_HALF,
		0x24, 0, 0.8f, -0.75f, stream);
	expect_near("native Y + X > 0.5 X", stream[0].position[0], 10.0f);
	expect_near("native Y + X > 0.5 Y", stream[0].position[1], 20.0f);
	run_native_handoff_mode(PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_GT_HALF,
		0x24, 0, 0.25f, -0.75f, stream);
	expect_near("native Y + X > 0.5 preserves small X",
		stream[0].position[0], 10.25f);
	expect_near("native Y + X > 0.5 still applies Y",
		stream[0].position[1], 20.0f);
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OFF);
	PGXP_DiagFrame(0);
}

static void test_exact_edge_adjacency(void)
{
	static const int32_t upper_native[3][2] = {
		{ 0, 0 }, { 10, 0 }, { 0, 10 }
	};
	static const float upper_precise[3][2] = {
		{ 0.0f, 0.0f }, { 10.0f, 0.0f }, { 0.0f, 10.0f }
	};
	static const int32_t lower_native[3][2] = {
		{ 10, 0 }, { 0, 0 }, { 10, -10 }
	};
	static const float lower_precise[3][2] = {
		{ 10.0f, -0.125f }, { 0.0f, -0.125f }, { 10.0f, -10.0f }
	};
	static const float lower_precise_consistent[3][2] = {
		{ 10.0f, 0.0f }, { 0.0f, 0.0f }, { 10.0f, -10.0f }
	};
	static const int32_t same_side_native[3][2] = {
		{ 10, 0 }, { 0, 0 }, { 10, 10 }
	};
	test_vertex stream[6];

	puts("[R7] exact native shared-edge masks honor topology and material");
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_ADJACENCY_MATERIAL_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], upper_native, upper_precise, 0x24);
	submit_triangle(&stream[3], lower_native, lower_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("same-material upper shared edge",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("same-material lower shared edge",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_ADJACENCY_MATERIAL_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], upper_native, upper_precise, 0x24);
	submit_material_key = 2;
	submit_triangle(&stream[3], lower_native, lower_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("material gate rejects upper",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("material gate rejects lower",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_ADJACENCY_ANY_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], upper_native, upper_precise, 0x24);
	submit_material_key = 2;
	submit_triangle(&stream[3], lower_native, lower_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("any-material accepts upper",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("any-material accepts lower",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_ADJACENCY_ANY_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], upper_native, upper_precise, 0x24);
	submit_triangle(&stream[3], same_side_native, lower_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("same-side overlap rejected upper",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("same-side overlap rejected lower",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_ADJACENCY_ANY_FOUR);
	submit_triangle(&stream[0], upper_native, upper_precise, 0x24);
	submit_triangle(&stream[3], lower_native, lower_precise_consistent, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("already-consistent upper stays clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("already-consistent lower stays clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	/* The frame-local hash deliberately spans command-buffer flushes.  Only
	 * the later buffer can be marked, because the earlier one has already
	 * been submitted to GL. */
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_ADJACENCY_MATERIAL_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], upper_native, upper_precise, 0x24);
	PGXP_DiagGLRepair(stream, 3, (unsigned)sizeof(stream[0]));
	expect_mask("first cross-buffer side initially clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	submit_triangle(&stream[0], lower_native, lower_precise, 0x24);
	PGXP_DiagGLRepair(stream, 3, (unsigned)sizeof(stream[0]));
	expect_mask("later cross-buffer side selected",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	submit_material_key = 1;
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OFF);
	PGXP_DiagFrame(0);
}

static void test_partial_edge_adjacency(void)
{
	static const int32_t long_native[3][2] = {
		{ 0, 0 }, { 10, 0 }, { 0, 10 }
	};
	static const float long_precise[3][2] = {
		{ 0.0f, 0.0f }, { 10.0f, 0.0f }, { 0.0f, 10.0f }
	};
	static const int32_t short_native[3][2] = {
		{ 5, 0 }, { 0, 0 }, { 5, -5 }
	};
	static const float short_precise[3][2] = {
		{ 5.0f, -0.125f }, { 0.0f, -0.125f }, { 5.0f, -5.0f }
	};
	static const float short_precise_consistent[3][2] = {
		{ 5.0f, 0.0f }, { 0.0f, 0.0f }, { 5.0f, -5.0f }
	};
	static const float short_precise_overlap[3][2] = {
		{ 5.0f, 0.125f }, { 0.0f, 0.125f }, { 5.0f, -5.0f }
	};
	static const float short_precise_mixed[3][2] = {
		{ 5.0f, 0.125f }, { 0.0f, -0.125f }, { 5.0f, -5.0f }
	};
	static const int32_t parallel_one_native[3][2] = {
		{ 5, -1 }, { 0, -1 }, { 5, -5 }
	};
	static const float parallel_one_precise[3][2] = {
		{ 5.0f, -0.125f }, { 0.0f, -0.125f }, { 5.0f, -5.0f }
	};
	static const int32_t parallel_two_native[3][2] = {
		{ 5, -2 }, { 0, -2 }, { 5, -5 }
	};
	static const int32_t parallel_fraction_long_native[3][2] = {
		{ 0, 0 }, { 3, 4 }, { -4, 3 }
	};
	static const float parallel_fraction_long_precise[3][2] = {
		{ 0.0f, 0.0f }, { 3.0f, 4.0f }, { -4.0f, 3.0f }
	};
	static const int32_t parallel_fraction_peer_native[3][2] = {
		{ 1, 1 }, { 4, 5 }, { 6, 0 }
	};
	static const float parallel_fraction_peer_precise[3][2] = {
		{ 0.94f, 1.045f }, { 3.94f, 5.045f }, { 6.0f, 0.0f }
	};
	static const int32_t short_same_side[3][2] = {
		{ 5, 0 }, { 0, 0 }, { 5, 5 }
	};
	static const int32_t crossing_native[3][2] = {
		{ 15, 0 }, { 5, 0 }, { 15, -5 }
	};
	static const float crossing_precise[3][2] = {
		{ 15.0f, -0.125f }, { 5.0f, -0.125f }, { 15.0f, -5.0f }
	};
	static const int32_t touch_native[3][2] = {
		{ 15, 0 }, { 10, 0 }, { 15, -5 }
	};
	static const float touch_precise[3][2] = {
		{ 15.0f, -0.125f }, { 10.0f, -0.125f }, { 15.0f, -5.0f }
	};
	static const int32_t equal_native[3][2] = {
		{ 10, 0 }, { 0, 0 }, { 10, -10 }
	};
	static const float equal_precise[3][2] = {
		{ 10.0f, -0.125f }, { 0.0f, -0.125f }, { 10.0f, -10.0f }
	};
	static const int32_t diagonal_long_native[3][2] = {
		{ 0, 0 }, { 10, 10 }, { 0, 10 }
	};
	static const float diagonal_long_precise[3][2] = {
		{ 0.0f, 0.0f }, { 10.0f, 10.0f }, { 0.0f, 10.0f }
	};
	static const int32_t diagonal_short_native[3][2] = {
		{ 5, 5 }, { 0, 0 }, { 5, 0 }
	};
	static const float diagonal_short_precise[3][2] = {
		{ 5.0f, 4.875f }, { 0.0f, -0.125f }, { 5.0f, 0.0f }
	};
	static const int32_t vertical_long_native[3][2] = {
		{ 0, 0 }, { 0, 10 }, { -10, 0 }
	};
	static const float vertical_long_precise[3][2] = {
		{ 0.0f, 0.0f }, { 0.0f, 10.0f }, { -10.0f, 0.0f }
	};
	static const int32_t vertical_short_native[3][2] = {
		{ 0, 5 }, { 0, 0 }, { 5, 5 }
	};
	static const float vertical_short_precise[3][2] = {
		{ 0.125f, 5.0f }, { 0.125f, 0.0f }, { 5.0f, 5.0f }
	};
	static const unsigned overlap_width_modes[] = {
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_OVERLAP_ONE,
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_OVERLAP_TWO,
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_OVERLAP_THREE
	};
	static const unsigned any_width_modes[] = {
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_ONE,
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_TWO,
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_THREE
	};
	test_vertex stream[6];
	unsigned mode_index;

	puts("[R8] partial native edge masks distinguish containment and overlap");
	PGXP_DiagGLSetMode(
		PGXP_DIAG_GL_TEST_PARTIAL_SHORT_MATERIAL_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("containment leaves long edge clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("containment selects short edge",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(
		PGXP_DIAG_GL_TEST_PARTIAL_SHORT_MATERIAL_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_material_key = 2;
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("partial material gate rejects long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("partial material gate rejects short",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_SHORT_ANY_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_material_key = 2;
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("partial any keeps long clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("partial any selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR);
	submit_material_key = 1;
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("partial both selects long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("partial both selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], crossing_native, crossing_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("containment mode rejects crossing long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("containment mode rejects crossing peer",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OVERLAP_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], crossing_native, crossing_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("overlap mode selects first edge",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("overlap mode selects crossing edge",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OVERLAP_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], touch_native, touch_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("point touch leaves first edge clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("point touch leaves peer edge clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OVERLAP_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], equal_native, equal_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("equal span stays in exact-edge family (first)",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("equal span stays in exact-edge family (peer)",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], diagonal_long_native,
		diagonal_long_precise, 0x24);
	submit_triangle(&stream[3], diagonal_short_native,
		diagonal_short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("diagonal containment selects long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("diagonal containment selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], vertical_long_native,
		vertical_long_precise, 0x24);
	submit_triangle(&stream[3], vertical_short_native,
		vertical_short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("vertical containment selects long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("vertical containment selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_LONG_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("long-only selects containing edge",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("long-only leaves contained edge clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap-only both selects long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("gap-only both selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_FOUR);
	submit_triangle(&stream[0], diagonal_long_native,
		diagonal_long_precise, 0x24);
	submit_triangle(&stream[3], diagonal_short_native,
		diagonal_short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap-only accepts diagonal long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("gap-only accepts diagonal short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_FOUR);
	submit_triangle(&stream[0], vertical_long_native,
		vertical_long_precise, 0x24);
	submit_triangle(&stream[3], vertical_short_native,
		vertical_short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap-only accepts vertical long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("gap-only accepts vertical short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_LONG_GAP_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap-only long selects containing edge",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("gap-only long leaves contained edge clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_OVERLAP_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("overlap-only rejects true gap long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("overlap-only rejects true gap short",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_OVERLAP_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native,
		short_precise_overlap, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("overlap-only selects overlapping long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("overlap-only selects overlapping short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	for (mode_index = 0;
	     mode_index < sizeof(overlap_width_modes) /
		     sizeof(overlap_width_modes[0]);
	     mode_index++)
	{
		PGXP_DiagGLSetMode(overlap_width_modes[mode_index]);
		submit_triangle(&stream[0], long_native, long_precise, 0x24);
		submit_triangle(&stream[3], short_native, short_precise, 0x24);
		PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
		expect_mask("overlap-width rejects true gap long",
			PGXP_DiagGLSharedEdgeMask(0), 0u);
		expect_mask("overlap-width rejects true gap short",
			PGXP_DiagGLSharedEdgeMask(3), 0u);
		PGXP_DiagFrame(0);

		PGXP_DiagGLSetMode(overlap_width_modes[mode_index]);
		submit_triangle(&stream[0], long_native, long_precise, 0x24);
		submit_triangle(&stream[3], short_native,
			short_precise_overlap, 0x24);
		PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
		expect_mask("overlap-width selects overlap long",
			PGXP_DiagGLSharedEdgeMask(0), 1u);
		expect_mask("overlap-width selects overlap short",
			PGXP_DiagGLSharedEdgeMask(3), 1u);
		PGXP_DiagFrame(0);
	}

	for (mode_index = 0;
	     mode_index < sizeof(any_width_modes) / sizeof(any_width_modes[0]);
	     mode_index++)
	{
		PGXP_DiagGLSetMode(any_width_modes[mode_index]);
		submit_triangle(&stream[0], long_native, long_precise, 0x24);
		submit_triangle(&stream[3], short_native, short_precise, 0x24);
		PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
		expect_mask("any-width selects true-gap long",
			PGXP_DiagGLSharedEdgeMask(0), 1u);
		expect_mask("any-width selects true-gap short",
			PGXP_DiagGLSharedEdgeMask(3), 1u);
		PGXP_DiagFrame(0);
	}

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FIT_QUARTER);
	submit_triangle(&stream[0], parallel_fraction_long_native,
		parallel_fraction_long_precise, 0x24);
	submit_triangle(&stream[3], parallel_fraction_peer_native,
		parallel_fraction_peer_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel quarter selects fractional native neighbor long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("parallel quarter selects fractional native neighbor peer",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	expect_near("parallel quarter fits half precise gap long",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.0625f);
	expect_near("parallel quarter fits half precise gap peer",
		PGXP_DiagGLSharedEdgeExpansion(3, 0), 0.0625f);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FIT_HALF);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], parallel_one_native,
		parallel_one_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel half rejects one-pixel native distance long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("parallel half rejects one-pixel native distance peer",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FIT_ONE);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], parallel_one_native,
		parallel_one_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel one selects native neighbor long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("parallel one selects native neighbor peer",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	expect_near("parallel one fits half precise gap long",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.0625f);
	expect_near("parallel one fits half precise gap peer",
		PGXP_DiagGLSharedEdgeExpansion(3, 0), 0.0625f);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FIT_ONE);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], parallel_two_native,
		parallel_one_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel one rejects two-pixel native distance long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("parallel one rejects two-pixel native distance peer",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FIT_TWO);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], parallel_two_native,
		parallel_one_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel two selects native neighbor long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("parallel two selects native neighbor peer",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	expect_near("parallel two fits half precise gap long",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.0625f);
	expect_near("parallel two fits half precise gap peer",
		PGXP_DiagGLSharedEdgeExpansion(3, 0), 0.0625f);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FOUR_ONE);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], parallel_one_native,
		parallel_one_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel fixed-four selects native neighbor long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("parallel fixed-four selects native neighbor peer",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	expect_near("parallel fixed-four has no fitted extra",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.0f);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARALLEL_GAP_FIT_ONE);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("parallel family excludes same-line long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("parallel family excludes same-line peer",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise_mixed, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap-only rejects mixed long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("gap-only rejects mixed short",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_OVERLAP_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise_mixed, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("overlap-only rejects mixed long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("overlap-only rejects mixed short",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_MIXED_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise_mixed, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("mixed-only selects mixed long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("mixed-only selects mixed short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_MIXED_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap+mixed selects true-gap long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("gap+mixed selects true-gap short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_MIXED_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native,
		short_precise_overlap, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("gap+mixed rejects overlap long",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("gap+mixed rejects overlap short",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_FIT);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("fitted both selects long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("fitted both selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	expect_near("fitted both long half-gap",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.0625f);
	expect_near("fitted both short half-gap",
		PGXP_DiagGLSharedEdgeExpansion(3, 0), 0.0625f);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_LONG_GAP_FIT);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("fitted long selects containing edge",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("fitted long leaves contained edge clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	expect_near("fitted long uses full gap",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.125f);
	expect_near("fitted long contained has no expansion",
		PGXP_DiagGLSharedEdgeExpansion(3, 0), 0.0f);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(
		PGXP_DIAG_GL_TEST_PARTIAL_BOTH_GAP_FIT_FLOOR4);
	PGXP_DiagGLRasterScale(2);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("floor-four fit selects long",
		PGXP_DiagGLSharedEdgeMask(0), 1u);
	expect_mask("floor-four fit selects short",
		PGXP_DiagGLSharedEdgeMask(3), 1u);
	expect_near("floor-four fit long minimum extra",
		PGXP_DiagGLSharedEdgeExpansion(0, 0), 0.09375f);
	expect_near("floor-four fit short minimum extra",
		PGXP_DiagGLSharedEdgeExpansion(3, 0), 0.09375f);
	PGXP_DiagFrame(0);
	PGXP_DiagGLRasterScale(1);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_same_side, short_precise, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("partial same-side leaves long clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("partial same-side leaves short clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);
	PGXP_DiagFrame(0);

	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR);
	submit_triangle(&stream[0], long_native, long_precise, 0x24);
	submit_triangle(&stream[3], short_native,
		short_precise_consistent, 0x24);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_mask("partial consistent long stays clear",
		PGXP_DiagGLSharedEdgeMask(0), 0u);
	expect_mask("partial consistent short stays clear",
		PGXP_DiagGLSharedEdgeMask(3), 0u);

	submit_material_key = 1;
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OFF);
	PGXP_DiagFrame(0);
}

int main(void)
{
	PGXP_Init();
	PGXP_DiagInit();
	PGXP_DiagGLRasterCaps(4);
	PGXP_SetModes(PGXP_MODE_MEMORY);
	test_opposing_tjunction_closes();
	test_conflicting_targets_are_atomic();
	test_pgxp_off_is_untouched();
	test_runtime_mode_matrix();
	test_exact_edge_adjacency();
	test_partial_edge_adjacency();
	PGXP_Shutdown();
	printf("\nFAIL count %d\n", failures);
	if (failures)
		return 1;
	puts("PASS");
	return 0;
}
