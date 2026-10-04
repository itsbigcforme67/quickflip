// PC tests for the QuickFlip core. Run with: bash tests/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/qflip.h"

void *qf_malloc(size_t n) { return malloc(n); }
void *qf_realloc(void *p, size_t n) { return realloc(p, n); }
void qf_free(void *p) { free(p); }

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t rng = 12345;
static int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 8) % (uint32_t)n); }

static int getpx(const QfFrame *f, int l, int x, int y) {
  return (f->px[l][y * QF_STRIDE + x / 8] >> (7 - x % 8)) & 1;
}

static void scribble(QfFrame *f, int strokes) {
  for (int s = 0; s < strokes; s++) {
    int l = rnd(2), x = rnd(QF_W), y = rnd(QF_H), size = 1 + rnd(8), pat = rnd(QF_PAT_COUNT);
    int mode = rnd(5) == 0 ? QF_ERASE : QF_DRAW;
    for (int k = 0; k < 10; k++) {
      int nx = x + rnd(41) - 20, ny = y + rnd(41) - 20;
      qf_line(f, l, x, y, nx, ny, size, pat, mode);
      x = nx; y = ny;
    }
  }
}

int main(int argc, char **argv) {
  static QfFrame a, b;

  // a dot of size 1 sets exactly one pixel; erase clears it
  qf_frame_clear(&a);
  CHECK(qf_frame_is_blank(&a));
  qf_dot(&a, 1, 10, 20, 1, QF_PAT_SOLID, QF_DRAW);
  CHECK(getpx(&a, 1, 10, 20) && !getpx(&a, 0, 10, 20) && !getpx(&a, 1, 11, 20));
  qf_dot(&a, 1, 10, 20, 3, QF_PAT_SOLID, QF_ERASE);
  CHECK(qf_frame_is_blank(&a));
  // dots off the edge do not write outside the frame
  qf_dot(&a, 0, -3, -3, 16, QF_PAT_SOLID, QF_DRAW);
  qf_dot(&a, 0, 259, 194, 16, QF_PAT_SOLID, QF_DRAW);
  CHECK(getpx(&a, 0, 0, 0) && getpx(&a, 0, 255, 191));

  // fill inside a box stays inside
  qf_frame_clear(&a);
  qf_line(&a, 0, 50, 50, 100, 50, 1, 0, QF_DRAW);
  qf_line(&a, 0, 100, 50, 100, 100, 1, 0, QF_DRAW);
  qf_line(&a, 0, 100, 100, 50, 100, 1, 0, QF_DRAW);
  qf_line(&a, 0, 50, 100, 50, 50, 1, 0, QF_DRAW);
  CHECK(qf_fill(&a, 0, 75, 75, QF_PAT_SOLID, QF_DRAW) == 0);
  CHECK(getpx(&a, 0, 51, 51) && getpx(&a, 0, 99, 99) && !getpx(&a, 0, 49, 75) && !getpx(&a, 0, 101, 75));
  CHECK(!getpx(&a, 1, 75, 75));
  int count = 0;
  for (int y = 0; y < QF_H; y++) for (int x = 0; x < QF_W; x++) count += getpx(&a, 0, x, y);
  CHECK(count == 51 * 51);
  // filling the outside with a pattern then erasing it all with a fill
  CHECK(qf_fill(&a, 0, 0, 0, QF_PAT_HALF, QF_DRAW) == 0);
  CHECK(getpx(&a, 0, 0, 0) && !getpx(&a, 0, 1, 0));
  // a fill on a busy random layer must finish
  qf_frame_clear(&b);
  for (int i = 0; i < QF_LAYER_BYTES; i++) b.px[0][i] = (uint8_t)rnd(256);
  CHECK(qf_fill(&b, 0, 128, 96, QF_PAT_SOLID, QF_DRAW) == 0);

  // a stand-alone frame round trip
  qf_frame_clear(&a);
  scribble(&a, 30);
  a.paper = QF_PAPER_BLACK; a.ink[0] = QF_INK_RED; a.ink[1] = QF_INK_BLUE;
  static uint8_t buf[QF_MAX_FRAME_BYTES];
  uint32_t n = qf_frame_encode(&a, 0, buf);
  memset(&b, 0x55, sizeof b);
  CHECK(qf_frame_decode(&b, buf, n) == n);
  CHECK(memcmp(&a, &b, sizeof a) == 0);
  // a full layer of 0xFF and of noise both round trip
  memset(a.px[0], 0xFF, QF_LAYER_BYTES);
  for (int i = 0; i < QF_LAYER_BYTES; i++) a.px[1][i] = (uint8_t)rnd(256);
  n = qf_frame_encode(&a, 0, buf);
  CHECK(n <= QF_MAX_FRAME_BYTES);
  CHECK(qf_frame_decode(&b, buf, n) == n && memcmp(&a, &b, sizeof a) == 0);

  // a note: 40 frames, each a small change on the last, plus edits
  QfNote note, back;
  qf_note_init(&note);
  qf_frame_clear(&a);
  for (int i = 0; i < 40; i++) {
    scribble(&a, 2);
    a.ink[0] = (uint8_t)(i % 3);
    CHECK(qf_note_insert(&note, note.count, &a) == 0);
  }
  qf_frame_clear(&b);
  CHECK(qf_note_insert(&note, 0, &b) == 0);  // a blank first frame
  CHECK(qf_note_move(&note, 0, 5) == 0);
  CHECK(qf_note_delete(&note, 7) == 0);
  CHECK(qf_note_set(&note, 2, &a) == 0);
  CHECK(note.count == 40);
  note.speed = 6; note.loop = 0; note.thumb = 3; note.created = 1790000000u;
  memcpy(note.author_id, "\1\2\3\4\5\6\7\x08", 8);
  strcpy(note.author, "Tester \xc3\xa9");

  uint8_t *file = 0;
  uint32_t flen = qf_note_save(&note, &file);
  CHECK(flen > 0);
  uint32_t raw = 0;
  for (int i = 0; i < note.count; i++) raw += note.frames[i].len;
  printf("40 frames: %u bytes stored separately, %u bytes as a file\n", raw, flen);
  CHECK(flen < raw);  // relative frames must have helped

  CHECK(qf_note_load(&back, file, flen) == 0);
  CHECK(back.count == note.count && back.speed == 6 && back.loop == 0 && back.thumb == 3);
  CHECK(back.created == 1790000000u && memcmp(back.author_id, note.author_id, 8) == 0);
  CHECK(strcmp(back.author, note.author) == 0);
  static QfFrame c, d;
  for (int i = 0; i < note.count; i++) {
    CHECK(qf_note_get(&note, i, &c) == 0 && qf_note_get(&back, i, &d) == 0);
    CHECK(memcmp(&c, &d, sizeof c) == 0);
  }
  QfNote hdr;
  CHECK(qf_note_peek(&hdr, file, flen) == 0 && hdr.speed == 6 && strcmp(hdr.author, note.author) == 0);

  if (argc > 1) {  // write the file for the Python checks
    FILE *fp = fopen(argv[1], "wb");
    fwrite(file, 1, flen, fp);
    fclose(fp);
    char path[512];
    snprintf(path, sizeof path, "%s.frames", argv[1]);
    fp = fopen(path, "wb");
    for (int i = 0; i < note.count; i++) {
      qf_note_get(&note, i, &c);
      static uint8_t img[QF_W * QF_H];
      qf_render(&c, 0, img);
      fwrite(img, 1, sizeof img, fp);
    }
    fclose(fp);
  }

  // damaged files must be refused or loaded, never crash (run under the sanitizers)
  uint8_t *bad = malloc(flen);
  int refused = 0;
  for (int t = 0; t < 3000; t++) {
    memcpy(bad, file, flen);
    int hits = 1 + rnd(8);
    for (int k = 0; k < hits; k++) bad[rnd(flen)] = (uint8_t)rnd(256);
    uint32_t cut = rnd(4) == 0 ? (uint32_t)rnd(flen) : flen;
    QfNote z;
    int r = qf_note_load(&z, bad, cut);
    refused += r != 0;
    if (r == 0) for (int i = 0; i < z.count; i++) CHECK(qf_note_get(&z, i, &c) == 0);
    qf_note_free(&z);
  }
  printf("damaged files: %d of 3000 refused\n", refused);
  free(bad);

  free(file);
  qf_note_free(&note);
  qf_note_free(&back);
  printf(fails ? "%d FAILED\n" : "ALL TESTS PASSED\n", fails);
  return fails != 0;
}
