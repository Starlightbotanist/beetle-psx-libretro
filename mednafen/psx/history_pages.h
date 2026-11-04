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
typedef struct
{
   HistoryPage **pages;
   size_t count;
} HistoryPages;
static HistoryPages history_current;
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
   for (i = 0; i < state->count; i++) history_page_release(state->pages[i]);
   free(state->pages);
   memset(state, 0, sizeof(*state));
}
static bool history_pages_save(HistoryPages *state, const void *data,
      size_t bytes)
{
   HistoryPages next;
   size_t i, j;
   next.count = bytes / (2 * HISTORY_WORDS);
   next.pages = (HistoryPage **)calloc(next.count, sizeof(*next.pages));
   if (!next.pages) return false;
   for (i = 0; i < next.count; i++)
   {
      HistoryPage *page = history_current.count == next.count ? history_current.pages[i] : NULL;
      const uint8_t *src = (const uint8_t *)data + i * 2 * HISTORY_WORDS;
      if (page && !checkpoint_dirty[i]) page->refs++;
      else
      {
         page = (HistoryPage *)calloc(1, sizeof(*page));
         if (!page) { history_pages_free(&next); return false; }
         page->refs = 1;
         memcpy(&page->solid, src, sizeof(page->solid));
         for (j = 1; j < HISTORY_WORDS; j++)
         {
            uint16_t word;
            memcpy(&word, src + 2 * j, sizeof(word));
            if (word != page->solid) break;
         }
         if (j != HISTORY_WORDS)
         {
            page->words = (uint16_t *)malloc(2 * HISTORY_WORDS);
            if (!page->words) { free(page); history_pages_free(&next); return false; }
            memcpy(page->words, src, 2 * HISTORY_WORDS);
         }
      }
      next.pages[i] = page;
   }
   history_pages_free(state);
   *state = next;
   history_pages_free(&history_current);
   history_current.count = next.count;
   history_current.pages = (HistoryPage **)calloc(next.count, sizeof(*next.pages));
   if (!history_current.pages) { history_current.count = 0; return true; }
   for (i = 0; i < next.count; i++) { history_current.pages[i] = next.pages[i]; next.pages[i]->refs++; }
   memset(checkpoint_dirty, 0, next.count);
   return true;
}
static void history_pages_load(const HistoryPages *state, void *data)
{
   size_t i, j;
   for (i = 0; i < state->count; i++)
   {
      HistoryPage *page = state->pages[i];
      uint8_t *dst = (uint8_t *)data + i * 2 * HISTORY_WORDS;
      const uint8_t *solid = (const uint8_t *)&page->solid;
      if (history_current.count == state->count && history_current.pages[i] == page && !checkpoint_dirty[i]) continue;
      if (page->words) memcpy(dst, page->words, 2 * HISTORY_WORDS);
      else if (solid[0] == solid[1]) memset(dst, solid[0], 2 * HISTORY_WORDS);
      else for (j = 0; j < HISTORY_WORDS; j++)
         memcpy(dst + 2 * j, &page->solid, sizeof(page->solid));
   }
   history_pages_free(&history_current);
   history_current.count = state->count;
   history_current.pages = (HistoryPage **)calloc(state->count, sizeof(*state->pages));
   if (!history_current.pages) { history_current.count = 0; return; }
   for (i = 0; i < state->count; i++) { history_current.pages[i] = state->pages[i]; state->pages[i]->refs++; }
   memset(checkpoint_dirty, 0, state->count);
}
#endif
