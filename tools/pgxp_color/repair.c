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
	PGXP_DiagGLPrimitive(output, 3, (unsigned)sizeof(output[0]));
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
	PGXP_SetModes(PGXP_MODE_NONE);
	submit_triangle(&stream[0], long_native, long_precise, 0x2c);
	submit_triangle(&stream[3], short_native, short_precise, 0x2c);
	PGXP_DiagGLRepair(stream, 6, (unsigned)sizeof(stream[0]));
	expect_near("PGXP-off junction point y", stream[3].position[1], -0.5f);
	expect_near("PGXP-off linked endpoint y", stream[4].position[1], -0.5f);
	PGXP_DiagFrame(0);
	PGXP_SetModes(PGXP_MODE_MEMORY);
}

int main(void)
{
	PGXP_Init();
	PGXP_DiagInit();
	PGXP_SetModes(PGXP_MODE_MEMORY);
	test_opposing_tjunction_closes();
	test_conflicting_targets_are_atomic();
	test_pgxp_off_is_untouched();
	PGXP_Shutdown();
	printf("\nFAIL count %d\n", failures);
	if (failures)
		return 1;
	puts("PASS");
	return 0;
}
