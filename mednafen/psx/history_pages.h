#ifndef PSX_HISTORY_PAGES_H
#define PSX_HISTORY_PAGES_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HISTORY_WORDS 8192

typedef struct HistoryPage
{
   unsigned refs;
   uint16_t solid;
   uint16_t *words;
} HistoryPage;

/* A saved state and the current-state cache can own the same immutable table. */
typedef struct HistoryPages
{
   unsigned refs;
   size_t count;
   HistoryPage *pages[];
} HistoryPages;

static HistoryPages *history_current;

static void history_page_release(HistoryPage *page)
{
   if (page && !--page->refs)
   {
      free(page->words);
      free(page);
   }
}

static void history_pages_free(HistoryPages *state)
{
   size_t i;
   if (!state || --state->refs)
      return;
   for (i = 0; i < state->count; i++)
      history_page_release(state->pages[i]);
   free(state);
}

static bool history_pages_save(HistoryPages **state, const void *data,
      size_t bytes)
{
   HistoryPages *next;
   size_t i, j, count = bytes / (2 * HISTORY_WORDS);

   if (count > (SIZE_MAX - sizeof(*next)) / sizeof(next->pages[0]))
      return false;
   next = (HistoryPages *)calloc(1, sizeof(*next) + count * sizeof(next->pages[0]));
   if (!next)
      return false;
   next->refs = 1;
   next->count = count;
   for (i = 0; i < count; i++)
   {
      HistoryPage *page = history_current && history_current->count == count ?
            history_current->pages[i] : NULL;
      const uint8_t *src = (const uint8_t *)data + i * 2 * HISTORY_WORDS;
      if (page && !snapshot_dirty[i])
         page->refs++;
      else
      {
         page = (HistoryPage *)calloc(1, sizeof(*page));
         if (!page)
         {
            history_pages_free(next);
            return false;
         }
         page->refs = 1;
         memcpy(&page->solid, src, sizeof(page->solid));
         for (j = 1; j < HISTORY_WORDS; j++)
         {
            uint16_t word;
            memcpy(&word, src + 2 * j, sizeof(word));
            if (word != page->solid)
               break;
         }
         if (j != HISTORY_WORDS)
         {
            page->words = (uint16_t *)malloc(2 * HISTORY_WORDS);
            if (!page->words)
            {
               free(page);
               history_pages_free(next);
               return false;
            }
            memcpy(page->words, src, 2 * HISTORY_WORDS);
         }
      }
      next->pages[i] = page;
   }
   history_pages_free(*state);
   *state = next;
   history_pages_free(history_current);
   history_current = next;
   next->refs++;
   memset(snapshot_dirty, 0, count);
   return true;
}

static void history_pages_load(HistoryPages *state, void *data)
{
   size_t i, j;
   for (i = 0; i < state->count; i++)
   {
      HistoryPage *page = state->pages[i];
      uint8_t *dst = (uint8_t *)data + i * 2 * HISTORY_WORDS;
      const uint8_t *solid = (const uint8_t *)&page->solid;
      if (history_current && history_current->count == state->count &&
            history_current->pages[i] == page && !snapshot_dirty[i])
         continue;
      if (page->words)
         memcpy(dst, page->words, 2 * HISTORY_WORDS);
      else if (solid[0] == solid[1])
         memset(dst, solid[0], 2 * HISTORY_WORDS);
      else
         for (j = 0; j < HISTORY_WORDS; j++)
            memcpy(dst + 2 * j, &page->solid, sizeof(page->solid));
   }
   state->refs++;
   history_pages_free(history_current);
   history_current = state;
   memset(snapshot_dirty, 0, state->count);
}

#endif
