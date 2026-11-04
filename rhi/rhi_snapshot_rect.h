#ifndef RHI_SNAPSHOT_RECT_H
#define RHI_SNAPSHOT_RECT_H

#include <stdint.h>
#include <retro_inline.h>

typedef struct
{
   uint8_t left, top, right, bottom;
} RHISnapshotRect;

/* Clip one axis of a wrapping write to this tile. If a write goes all
 * the way around into the first tile, conservatively cover that tile. */
static INLINE void rhi_snapshot_axis(unsigned offset, unsigned length,
      unsigned tile, unsigned size, unsigned period, unsigned *lo, unsigned *hi)
{
   unsigned begin = tile * size;
   unsigned end = offset + length;
   *lo = offset > begin ? offset - begin : 0;
   *hi = end - begin < size ? end - begin : size;
   if (!tile && end > period)
   {
      *lo = 0;
      *hi = size;
   }
}

static INLINE void rhi_snapshot_mark(RHISnapshotRect *rect, unsigned x, unsigned y,
      unsigned w, unsigned h, unsigned i, unsigned j, unsigned size)
{
   unsigned left, top, right, bottom;
   rhi_snapshot_axis(x % size, w, i, size, 1024, &left, &right);
   rhi_snapshot_axis(y % size, h, j, size, 512, &top, &bottom);
   if (!rect->right || left < rect->left)
      rect->left = left;
   if (!rect->bottom || top < rect->top)
      rect->top = top;
   if (right > rect->right)
      rect->right = right;
   if (bottom > rect->bottom)
      rect->bottom = bottom;
}

#endif
