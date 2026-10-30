#ifndef PGXP_STATE_H
#define PGXP_STATE_H

#include <stddef.h>
#include <string.h>
#include <boolean.h>
#include <stdint.h>

/* Private runtime snapshots; these bytes are never part of a disk state. */
void *PGXP_StateCreate(void);
void PGXP_StateFree(void *state);
bool PGXP_StateMatches(const void *state);
bool PGXP_StateSave(void *state);
bool PGXP_StateLoad(const void *state);
bool PGXP_StatePrepare(void *state);
void *PGXP_VertexStateCreate(void);
void PGXP_VertexStateFree(void *state);
bool PGXP_VertexStateSave(void *state);
bool PGXP_VertexStatePrepare(void *state);
void PGXP_VertexStateLoad(void *state);

void *PGXP_MemoryStateCreate(void);
void PGXP_MemoryStateFree(void *state);
bool PGXP_MemoryStateSave(void *state);
void PGXP_MemoryStateLoad(const void *state);
size_t PGXP_CPUStateSize(void);
size_t PGXP_GTEStateSize(void);
size_t PGXP_GPUStateSize(void);
void PGXP_CPUState(void *data, bool load);
void PGXP_GTEState(void *data, bool load);
void PGXP_GPUState(void *data, bool load);

static inline void PGXP_StateCopy(uint8_t **cursor, void *value,
      size_t size, bool load)
{
   if (load)
      memcpy(value, *cursor, size);
   else
      memcpy(*cursor, value, size);
   *cursor += size;
}

#endif
