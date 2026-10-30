#ifndef __LIBRETRO_CBS_H
#define __LIBRETRO_CBS_H

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

extern retro_video_refresh_t video_cb;
extern retro_environment_t environ_cb;
bool retro_is_speculative(void);
bool retro_using_memory_snapshots(void);
void retro_invalidate_memory_snapshots(void);
void retro_mark_memory_restore_failed(void);

#ifdef __cplusplus
}
#endif

#endif
