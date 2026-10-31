/* No GL context needed: exercise the shipping helper with controlled error
 * flags, including a driver that keeps returning CONTEXT_LOST forever. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned GLenum;
typedef struct { bool snapshot_failed; } gl_renderer;
static struct { GLenum (*fp_glGetGraphicsResetStatus)(void); } gl_caps;
static bool memory_restore_failed;
static void retro_mark_memory_restore_failed(void) { memory_restore_failed = true; }
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_CONTEXT_LOST 0x0507

static GLenum queue[4], persistent;
static unsigned position, length, queries;
static GLenum reset_status;
static unsigned reset_queries;
static GLenum reset_status_query(void)
{
   reset_queries++;
   return reset_status;
}
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "gl_errors_test:%u: %s\n", __LINE__, #condition); exit(1); } } while (0)

static GLenum glGetError(void)
{
   /* A missing stop on a persistent fatal error fails rather than hanging CI.
    * This is a fixture guard, not a polling limit in the renderer. */
   CHECK(++queries <= 16);
   return position < length ? queue[position++] : persistent;
}
#include "build/gl_errors_fragment.h"

static void run(bool inherited, GLenum first, GLenum second,
      GLenum repeating, bool expected, bool fatal, unsigned expected_queries)
{
   gl_renderer renderer = { false };
   memory_restore_failed = false;
   position = queries = length = 0;
   if (first) queue[length++] = first;
   if (second) queue[length++] = second;
   persistent = repeating;
   CHECK(gl_snapshot_check_errors(&renderer, inherited) == expected);
   CHECK(renderer.snapshot_failed == fatal);
   CHECK(memory_restore_failed == fatal);
   CHECK(queries == expected_queries);
}

int main(void)
{
   gl_renderer renderer = { false };
   run(true, 0, 0, 0, true, false, 1);
   run(true, GL_INVALID_ENUM, GL_INVALID_OPERATION, 0, true, false, 3);
   run(false, GL_INVALID_ENUM, GL_INVALID_OPERATION, 0, false, false, 3);
   run(true, 0, 0, GL_CONTEXT_LOST, false, true, 1);
   run(false, 0, 0, GL_CONTEXT_LOST, false, true, 1);
   run(true, GL_INVALID_ENUM, GL_INVALID_OPERATION, GL_CONTEXT_LOST, false, true, 3);
   run(true, 0, 0, GL_OUT_OF_MEMORY, false, true, 1);
   run(false, GL_INVALID_ENUM, 0, GL_OUT_OF_MEMORY, false, true, 2);
   run(true, 0, 0, GL_INVALID_OPERATION, false, false, 16);
   run(false, 0, 0, GL_INVALID_OPERATION, false, false, 16);
   gl_caps.fp_glGetGraphicsResetStatus = reset_status_query;
   reset_status = 0x8255; /* Actual reset, with no GL error flag. */
   queries = reset_queries = 0;
   CHECK(!gl_snapshot_check_errors(&renderer, true));
   CHECK(renderer.snapshot_failed && reset_queries == 1 && queries == 0);
   renderer.snapshot_failed = false;
   CHECK(!gl_snapshot_check_errors(&renderer, false));
   CHECK(renderer.snapshot_failed && reset_queries == 2 && queries == 0);
   reset_status = GL_NO_ERROR;
   run(true, GL_INVALID_ENUM, 0, 0, true, false, 2);
   puts("PASS GL error scopes: persistent context loss stops on its first report");
   return 0;
}
