// QuickFlip core: flipbook animation notes (frames, drawing tools, file format).
// Plain C99 with no library calls, so the same files build for the web (wasm), the ESP32 and the
// PC tests. Memory comes from qf_malloc / qf_realloc / qf_free, which each host defines.
//
// A frame is 256x192 with two 1-bit layers. Layer 0 is drawn over layer 1. Each layer has an
// ink colour, and the frame has a paper colour. Pixels are packed 8 to a byte, leftmost pixel in
// the top bit, 32 bytes per row. The file format is described in FORMAT.md.
#ifndef QFLIP_H
#define QFLIP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QF_W 256
#define QF_H 192
#define QF_STRIDE 32
#define QF_LAYER_BYTES (QF_STRIDE * QF_H)
#define QF_MAX_FRAMES 999
#define QF_MAX_AUTHOR 32
#define QF_MAX_FRAME_BYTES (1 + 2 * (48 + QF_LAYER_BYTES))

enum { QF_PAPER_WHITE = 0, QF_PAPER_BLACK = 1 };
// AUTO is black on white paper and white on black paper.
enum { QF_INK_AUTO = 0, QF_INK_RED = 1, QF_INK_BLUE = 2 };
enum { QF_PAT_SOLID = 0, QF_PAT_HALF = 1, QF_PAT_LIGHT = 2, QF_PAT_LINES = 3, QF_PAT_COUNT = 4 };
enum { QF_DRAW = 0, QF_ERASE = 1 };

// Palette indices written by the render functions. 0-3 are the colours themselves, 4-7 are the
// same colours faded, for the onion skin (the previous frame shown faintly while drawing).
enum { QF_COL_WHITE = 0, QF_COL_BLACK = 1, QF_COL_RED = 2, QF_COL_BLUE = 3, QF_COL_FADED = 4 };

// Speeds 1..8, frames per second times 2 (so speed 1 is half a frame per second).
extern const uint8_t qf_speed_fps2[9];

typedef struct {
  uint8_t px[2][QF_LAYER_BYTES];
  uint8_t paper;
  uint8_t ink[2];
} QfFrame;

typedef struct {
  uint8_t *data;
  uint32_t len;
} QfStored;

typedef struct {
  QfStored *frames;  // each frame encoded on its own (never relative to another frame)
  uint16_t count, cap;
  uint8_t speed;     // 1..8
  uint8_t loop;      // 1 = play over and over
  uint16_t thumb;    // frame shown in galleries
  uint32_t created;  // unix time, 0 = unknown
  uint8_t author_id[8];
  char author[QF_MAX_AUTHOR + 1];  // UTF-8
} QfNote;

// Supplied by the host.
void *qf_malloc(size_t n);
void *qf_realloc(void *p, size_t n);
void qf_free(void *p);

// ---- frames and drawing ----
void qf_frame_clear(QfFrame *f);  // empty layers, white paper, AUTO ink
int qf_frame_is_blank(const QfFrame *f);
void qf_layer_clear(QfFrame *f, int layer);
// Stamp a round brush (diameter 1..16) through the pattern. mode is QF_DRAW or QF_ERASE.
void qf_dot(QfFrame *f, int layer, int x, int y, int size, int pattern, int mode);
void qf_line(QfFrame *f, int layer, int x0, int y0, int x1, int y1, int size, int pattern, int mode);
// Fill the connected area of the layer around (x, y) that has the same value as that pixel.
// Returns 0, or -1 if out of memory.
int qf_fill(QfFrame *f, int layer, int x, int y, int pattern, int mode);
// Draw one row (or the whole frame) as palette indices. onion may be NULL.
void qf_render_row(const QfFrame *f, const QfFrame *onion, int y, uint8_t out[QF_W]);
void qf_render(const QfFrame *f, const QfFrame *onion, uint8_t out[QF_W * QF_H]);
// 64x48 thumbnail: each pixel is the most common colour of a 4x4 block.
void qf_render_thumb(const QfFrame *f, uint8_t out[64 * 48]);

// ---- one frame on its own ----
// Encode f (relative to prev if prev is not NULL). out needs QF_MAX_FRAME_BYTES. Returns length.
uint32_t qf_frame_encode(const QfFrame *f, const QfFrame *prev, uint8_t *out);
// Decode into f. If the frame is relative, f must already hold the previous frame.
// Returns bytes used, or 0 if the data is bad.
uint32_t qf_frame_decode(QfFrame *f, const uint8_t *data, uint32_t len);

// ---- notes ----
void qf_note_init(QfNote *n);  // empty note (no frames), speed 4, loop on
void qf_note_free(QfNote *n);  // frees the frames; the note can be used again after qf_note_init
// Frame i into f. Returns 0, or -1 if i is out of range or the stored data is bad.
int qf_note_get(const QfNote *n, int i, QfFrame *f);
// Replace frame i, or insert a new frame before i (i == count appends). Returns 0 or -1.
int qf_note_set(QfNote *n, int i, const QfFrame *f);
int qf_note_insert(QfNote *n, int i, const QfFrame *f);
int qf_note_delete(QfNote *n, int i);
int qf_note_move(QfNote *n, int from, int to);
// Load a .qflip file. Returns 0, or a negative error (the note is left empty).
int qf_note_load(QfNote *n, const uint8_t *data, uint32_t len);
// Write a .qflip file into memory from qf_malloc (the caller frees it). Returns length, 0 on error.
uint32_t qf_note_save(const QfNote *n, uint8_t **out);
// The frame count and header fields of a file without loading the frames. Returns 0 or error.
int qf_note_peek(QfNote *hdr, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif
