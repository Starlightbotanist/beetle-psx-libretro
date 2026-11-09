#ifndef RHI_PGXP_DEPTH_H
#define RHI_PGXP_DEPTH_H

#include <boolean.h>
#include <retro_inline.h>
#include <stdint.h>

typedef struct rhi_pgxp_depth_state
{
   bool active;
   float maximum;
} rhi_pgxp_depth_state;

/* A long run of descending, empty ordering-table buckets separates draw
 * ranges the guest explicitly ordered. The last empty entry may point to
 * the first payload instead of the next empty bucket. */
static INLINE bool rhi_pgxp_depth_ordering_boundary(uint8_t *empty_links,
      uint32_t address, uint32_t header)
{
   enum { minimum_gap = 24, redirect = minimum_gap + 1 };

   if (header >> 24)
   {
      const bool boundary = *empty_links >= minimum_gap;
      *empty_links = 0;
      return boundary;
   }

   if (address >= 4 && !(address & 3) && header == address - 4)
   {
      if (*empty_links == redirect)
         *empty_links = 1;
      else if (*empty_links < minimum_gap)
         ++*empty_links;
   }
   else
      *empty_links = *empty_links == minimum_gap ? redirect : 0;
   return false;
}

/* Unit W denotes unavailable depth. Values outside the native depth range
 * retain painter ordering rather than clipping valid far geometry. */
static INLINE bool rhi_pgxp_depth_valid(const float *w, unsigned count,
      bool allowed, float *maximum)
{
   unsigned i;
   if (!allowed || count < 3 || count > 4)
      return false;
   *maximum = w[0];
   for (i = 0; i < count; i++)
   {
      if (!(w[i] > 1.f && w[i] <= 65535.f))
         return false;
      if (w[i] > *maximum)
         *maximum = w[i];
   }
   return true;
}

/* PS1 display lists can change projection space mid-frame. Begin a new group
 * when a polygon extends sufficiently beyond the queued depth range. Its near
 * vertices can still overlap the old range, so testing only them misses sky
 * to world transitions as the camera moves. Retain the maximum of the actual
 * queue to avoid repeated splits within its range. Preserve the boundary
 * before committing the next group. */
static INLINE bool rhi_pgxp_depth_flush(const rhi_pgxp_depth_state *state,
      bool active, float maximum)
{
   return active != state->active || (active && state->maximum > 0.f &&
         maximum > state->maximum + 300.f);
}

#endif
