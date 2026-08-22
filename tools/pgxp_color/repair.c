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

static void submit_triangle(test_vertex output[3],
		const int32_t native[3][2], const float precise[3][2],
		uint8_t opcode)
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
	PGXP_DiagSubmitPrimitive(submitted, 3, 0, 0);
	PGXP_DiagGLPrimitive(output, 3, (unsigned)sizeof(output[0]),
		submit_material_key);
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
	     mode < PGXP_DIAG_GL_TEST_COUNT; mode++)
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
	PGXP_DiagGLSetMode(PGXP_DIAG_GL_TEST_OFF);
	PGXP_DiagFrame(0);
}

int main(void)
{
	PGXP_Init();
	PGXP_DiagInit();
	PGXP_SetModes(PGXP_MODE_MEMORY);
	test_opposing_tjunction_closes();
	test_conflicting_targets_are_atomic();
	test_pgxp_off_is_untouched();
	test_runtime_mode_matrix();
	PGXP_Shutdown();
	printf("\nFAIL count %d\n", failures);
	if (failures)
		return 1;
	puts("PASS");
	return 0;
}
