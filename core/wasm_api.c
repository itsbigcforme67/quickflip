// The web app's view of the core: one note being edited and a few frame slots.
// Built to web/core/qflip.wasm by build.sh. No C library, so this file also supplies memory
// allocation and the mem* functions the compiler may call.
#include "qflip.h"

#define EXPORT __attribute__((visibility("default")))

// ---------------------------------------------------------------- allocator
// First fit over a list of blocks in address order, merging free neighbours. Plenty for a
// few thousand frames.
extern unsigned char __heap_base;
typedef struct Blk { size_t size; struct Blk *next; int free; int pad; } Blk;
static Blk *heap_first, *heap_last;
static uint8_t *heap_end;

static size_t al16(size_t n) { return (n + 15) & ~(size_t)15; }

void *memset(void *d, int v, size_t n) {
  uint8_t *p = (uint8_t *)d;
  while (n--) *p++ = (uint8_t)v;
  return d;
}
void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *p = (uint8_t *)d;
  const uint8_t *q = (const uint8_t *)s;
  while (n--) *p++ = *q++;
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  uint8_t *p = (uint8_t *)d;
  const uint8_t *q = (const uint8_t *)s;
  if (p < q) while (n--) *p++ = *q++;
  else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}

void *qf_malloc(size_t n) {
  if (!n) n = 1;
  n = al16(n);
  for (Blk *b = heap_first; b; b = b->next) {
    if (!b->free || b->size < n) continue;
    if (b->size >= n + sizeof(Blk) + 64) {
      Blk *r = (Blk *)((uint8_t *)(b + 1) + n);
      r->size = b->size - n - sizeof(Blk);
      r->next = b->next;
      r->free = 1;
      b->next = r;
      b->size = n;
      if (heap_last == b) heap_last = r;
    }
    b->free = 0;
    return b + 1;
  }
  if (!heap_end) heap_end = (uint8_t *)al16((size_t)&__heap_base);
  uint8_t *top = heap_end;
  size_t need = sizeof(Blk) + n;
  size_t have = (size_t)__builtin_wasm_memory_size(0) * 65536;
  if ((size_t)top + need > have) {
    size_t pages = ((size_t)top + need - have + 65535) / 65536;
    if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return 0;
  }
  Blk *b = (Blk *)top;
  b->size = n;
  b->next = 0;
  b->free = 0;
  if (heap_last) heap_last->next = b;
  else heap_first = b;
  heap_last = b;
  heap_end = top + need;
  return b + 1;
}

void qf_free(void *p) {
  if (!p) return;
  Blk *b = (Blk *)p - 1;
  b->free = 1;
  // merge runs of free blocks (the list is in address order and has no gaps)
  for (Blk *c = heap_first; c; c = c->next)
    while (c->free && c->next && c->next->free) {
      if (heap_last == c->next) heap_last = c;
      c->size += sizeof(Blk) + c->next->size;
      c->next = c->next->next;
    }
}

void *qf_realloc(void *p, size_t n) {
  if (!p) return qf_malloc(n);
  Blk *b = (Blk *)p - 1;
  if (b->size >= n) return p;
  void *q = qf_malloc(n);
  if (!q) return 0;
  memcpy(q, p, b->size);
  qf_free(p);
  return q;
}

// ---------------------------------------------------------------- the note and frame slots
// Slots: 0 the frame being drawn, 1 the onion skin, 2 and 3 free for the page (playback,
// copy and paste). The page reads and writes the slot memory directly.

static QfNote note;
static QfFrame slot[4];
static uint8_t pixels[QF_W * QF_H];
static uint8_t thumb[64 * 48];
static uint8_t *saved;

EXPORT void *api_slot(int s) { return &slot[s & 3]; }
EXPORT int api_slot_size(void) { return (int)sizeof(QfFrame); }
EXPORT void *api_note(void) { return &note; }  // see app.js for the field offsets
EXPORT void *api_alloc(int n) { return qf_malloc((size_t)n); }
EXPORT void api_release(void *p) { qf_free(p); }

EXPORT void api_new(void) {
  qf_note_free(&note);
  qf_note_init(&note);
  qf_frame_clear(&slot[0]);
  qf_note_insert(&note, 0, &slot[0]);
}
EXPORT int api_load(const uint8_t *data, int len) {
  qf_note_free(&note);
  return qf_note_load(&note, data, (uint32_t)len);
}
EXPORT int api_save(void) {
  qf_free(saved);
  saved = 0;
  return (int)qf_note_save(&note, &saved);
}
EXPORT void *api_saved(void) { return saved; }

EXPORT int api_count(void) { return note.count; }
EXPORT int api_get(int i, int s) { return qf_note_get(&note, i, &slot[s & 3]); }
EXPORT int api_set(int i, int s) { return qf_note_set(&note, i, &slot[s & 3]); }
EXPORT int api_insert(int i, int s) { return qf_note_insert(&note, i, &slot[s & 3]); }
EXPORT int api_delete(int i) { return qf_note_delete(&note, i); }
EXPORT int api_move(int a, int b) { return qf_note_move(&note, a, b); }
EXPORT int api_bytes(void) {
  int t = 0;
  for (int i = 0; i < note.count; i++) t += (int)note.frames[i].len;
  return t;
}

EXPORT void api_clear(int s) { qf_frame_clear(&slot[s & 3]); }
EXPORT void api_clear_layer(int s, int layer) { qf_layer_clear(&slot[s & 3], layer); }
EXPORT int api_blank(int s) { return qf_frame_is_blank(&slot[s & 3]); }
EXPORT void api_line(int s, int layer, int x0, int y0, int x1, int y1, int size, int pat, int mode) {
  qf_line(&slot[s & 3], layer, x0, y0, x1, y1, size, pat, mode);
}
EXPORT int api_fill(int s, int layer, int x, int y, int pat, int mode) {
  return qf_fill(&slot[s & 3], layer, x, y, pat, mode);
}
// Render slot s, with slot o as the onion skin (o < 0 for none). Returns the 256x192 indices.
EXPORT void *api_render(int s, int o) {
  qf_render(&slot[s & 3], o < 0 ? 0 : &slot[o & 3], pixels);
  return pixels;
}
EXPORT void *api_thumb(int s) {
  qf_render_thumb(&slot[s & 3], thumb);
  return thumb;
}
