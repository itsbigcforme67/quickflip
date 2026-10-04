// QuickFlip core. See qflip.h and FORMAT.md.
#include "qflip.h"

const uint8_t qf_speed_fps2[9] = {0, 1, 2, 4, 8, 12, 24, 40, 60};

static void mset(void *d, int v, size_t n) {
  uint8_t *p = (uint8_t *)d;
  while (n--) *p++ = (uint8_t)v;
}
static void mcpy(void *d, const void *s, size_t n) {
  uint8_t *p = (uint8_t *)d;
  const uint8_t *q = (const uint8_t *)s;
  while (n--) *p++ = *q++;
}

// ---------------------------------------------------------------- frames and drawing

void qf_frame_clear(QfFrame *f) { mset(f, 0, sizeof *f); }

void qf_layer_clear(QfFrame *f, int layer) { mset(f->px[layer & 1], 0, QF_LAYER_BYTES); }

int qf_frame_is_blank(const QfFrame *f) {
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < QF_LAYER_BYTES; i++)
      if (f->px[l][i]) return 0;
  return 1;
}

static int pat_on(int pattern, int x, int y) {
  switch (pattern) {
    case QF_PAT_HALF: return ((x + y) & 1) == 0;
    case QF_PAT_LIGHT: return (y & 1) == 0 && ((x + (y & 2)) & 3) == 0;
    case QF_PAT_LINES: return (y & 1) == 0;
    default: return 1;
  }
}

static inline int px_get(const uint8_t *l, int x, int y) {
  return (l[y * QF_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
}
static inline void px_put(uint8_t *l, int x, int y, int v) {
  uint8_t m = (uint8_t)(0x80 >> (x & 7));
  if (v) l[y * QF_STRIDE + (x >> 3)] |= m;
  else l[y * QF_STRIDE + (x >> 3)] &= (uint8_t)~m;
}

void qf_dot(QfFrame *f, int layer, int x, int y, int size, int pattern, int mode) {
  if (size < 1) size = 1;
  if (size > 16) size = 16;
  uint8_t *l = f->px[layer & 1];
  int x0 = x - (size - 1) / 2, y0 = y - (size - 1) / 2;
  for (int j = 0; j < size; j++) {
    int py = y0 + j;
    if (py < 0 || py >= QF_H) continue;
    int dj = 2 * j - (size - 1);
    for (int i = 0; i < size; i++) {
      int px = x0 + i;
      if (px < 0 || px >= QF_W) continue;
      int di = 2 * i - (size - 1);
      if (di * di + dj * dj > size * size) continue;
      if (!pat_on(pattern, px, py)) continue;
      px_put(l, px, py, mode == QF_DRAW);
    }
  }
}

void qf_line(QfFrame *f, int layer, int x0, int y0, int x1, int y1, int size, int pattern, int mode) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 4096; guard++) {
    qf_dot(f, layer, x0, y0, size, pattern, mode);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

int qf_fill(QfFrame *f, int layer, int x, int y, int pattern, int mode) {
  if (x < 0 || y < 0 || x >= QF_W || y >= QF_H) return 0;
  uint8_t *l = f->px[layer & 1];
  int v = px_get(l, x, y);
  uint8_t *seen = (uint8_t *)qf_malloc(QF_LAYER_BYTES);
  int cap = 256, sp = 0;
  int16_t *st = (int16_t *)qf_malloc(sizeof(int16_t) * 2 * cap);
  if (!seen || !st) { qf_free(seen); qf_free(st); return -1; }
  mset(seen, 0, QF_LAYER_BYTES);
  st[sp++] = (int16_t)x; st[sp++] = (int16_t)y;
  while (sp) {
    int cy = st[--sp], cx = st[--sp];
    if (px_get(seen, cx, cy) || px_get(l, cx, cy) != v) continue;
    int lx = cx, rx = cx;
    while (lx > 0 && !px_get(seen, lx - 1, cy) && px_get(l, lx - 1, cy) == v) lx--;
    while (rx < QF_W - 1 && !px_get(seen, rx + 1, cy) && px_get(l, rx + 1, cy) == v) rx++;
    for (int i = lx; i <= rx; i++) px_put(seen, i, cy, 1);
    for (int ny = cy - 1; ny <= cy + 1; ny += 2) {
      if (ny < 0 || ny >= QF_H) continue;
      int in = 0;
      for (int i = lx; i <= rx; i++) {
        int ok = !px_get(seen, i, ny) && px_get(l, i, ny) == v;
        if (ok && !in) {
          if (sp + 2 > cap * 2) {
            int16_t *ns = (int16_t *)qf_realloc(st, sizeof(int16_t) * 4 * cap);
            if (!ns) { qf_free(seen); qf_free(st); return -1; }
            st = ns; cap *= 2;
          }
          st[sp++] = (int16_t)i; st[sp++] = (int16_t)ny;
        }
        in = ok;
      }
    }
  }
  for (int yy = 0; yy < QF_H; yy++)
    for (int xx = 0; xx < QF_W; xx++) {
      if (!px_get(seen, xx, yy)) continue;
      int on = pat_on(pattern, xx, yy);
      if (mode == QF_DRAW) px_put(l, xx, yy, on);
      else if (on) px_put(l, xx, yy, 0);
    }
  qf_free(seen);
  qf_free(st);
  return 0;
}

static int ink_col(int paper, int ink) {
  if (ink == QF_INK_RED) return QF_COL_RED;
  if (ink == QF_INK_BLUE) return QF_COL_BLUE;
  return paper ? QF_COL_WHITE : QF_COL_BLACK;
}

void qf_render_row(const QfFrame *f, const QfFrame *onion, int y, uint8_t out[QF_W]) {
  int paper = f->paper ? QF_COL_BLACK : QF_COL_WHITE;
  int c0 = ink_col(f->paper, f->ink[0]), c1 = ink_col(f->paper, f->ink[1]);
  int o0 = 0, o1 = 0;
  if (onion) {
    // the onion frame's ink, but judged against this frame's paper
    o0 = QF_COL_FADED + ink_col(f->paper, onion->ink[0]);
    o1 = QF_COL_FADED + ink_col(f->paper, onion->ink[1]);
  }
  const uint8_t *a = f->px[0] + y * QF_STRIDE, *b = f->px[1] + y * QF_STRIDE;
  const uint8_t *oa = onion ? onion->px[0] + y * QF_STRIDE : 0;
  const uint8_t *ob = onion ? onion->px[1] + y * QF_STRIDE : 0;
  for (int bx = 0; bx < QF_STRIDE; bx++) {
    uint8_t A = a[bx], B = b[bx], OA = oa ? oa[bx] : 0, OB = ob ? ob[bx] : 0;
    for (int k = 0; k < 8; k++) {
      uint8_t m = (uint8_t)(0x80 >> k);
      int c = paper;
      if (A & m) c = c0;
      else if (B & m) c = c1;
      else if (OA & m) c = o0;
      else if (OB & m) c = o1;
      out[bx * 8 + k] = (uint8_t)c;
    }
  }
}

void qf_render(const QfFrame *f, const QfFrame *onion, uint8_t out[QF_W * QF_H]) {
  for (int y = 0; y < QF_H; y++) qf_render_row(f, onion, y, out + y * QF_W);
}

void qf_render_thumb(const QfFrame *f, uint8_t out[64 * 48]) {
  uint8_t rows[4][QF_W];
  for (int ty = 0; ty < 48; ty++) {
    for (int r = 0; r < 4; r++) qf_render_row(f, 0, ty * 4 + r, rows[r]);
    for (int tx = 0; tx < 64; tx++) {
      uint8_t n[4] = {0, 0, 0, 0};
      for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) n[rows[r][tx * 4 + k] & 3]++;
      // any ink at all beats the paper, so thin lines still show
      int paper = f->paper ? QF_COL_BLACK : QF_COL_WHITE, best = paper, bn = 0;
      for (int c = 0; c < 4; c++)
        if (c != paper && n[c] > bn) { best = c; bn = n[c]; }
      if (bn < 3) best = paper;
      out[ty * 64 + tx] = (uint8_t)best;
    }
  }
}

// ---------------------------------------------------------------- frame encoding
// Frame: one flags byte, then for each layer not marked empty: 48 bytes of row types (2 bits per
// row, 4 rows per byte, first row in the top bits), then the rows:
//   0 all zero (no data)   1 4-byte mask of non-zero bytes, then those bytes
//   2 4-byte mask of bytes that are not 0xFF, then those bytes   3 the 32 bytes as they are
// Flags: bit 0 relative (layers are XORed onto the previous frame), bit 1 black paper,
// bits 2-3 layer 0 ink, bits 4-5 layer 1 ink, bit 6 layer 0 empty, bit 7 layer 1 empty.

static uint32_t enc_layer(const uint8_t *src, const uint8_t *prev, uint8_t *out) {
  uint8_t *types = out;
  uint8_t *p = out + 48;
  mset(types, 0, 48);
  for (int y = 0; y < QF_H; y++) {
    uint8_t row[QF_STRIDE];
    for (int i = 0; i < QF_STRIDE; i++)
      row[i] = (uint8_t)(src[y * QF_STRIDE + i] ^ (prev ? prev[y * QF_STRIDE + i] : 0));
    int nz = 0, nff = 0;
    for (int i = 0; i < QF_STRIDE; i++) { nz += row[i] != 0; nff += row[i] != 0xFF; }
    int t;
    if (nz == 0) t = 0;
    else if (4 + nz <= 4 + nff && 4 + nz < QF_STRIDE) t = 1;
    else if (4 + nff < QF_STRIDE) t = 2;
    else t = 3;
    types[y >> 2] |= (uint8_t)(t << (6 - 2 * (y & 3)));
    if (t == 1 || t == 2) {
      uint8_t skip = t == 1 ? 0 : 0xFF;
      uint32_t mask = 0;
      for (int i = 0; i < QF_STRIDE; i++)
        if (row[i] != skip) mask |= 0x80000000u >> i;
      *p++ = (uint8_t)(mask >> 24); *p++ = (uint8_t)(mask >> 16);
      *p++ = (uint8_t)(mask >> 8); *p++ = (uint8_t)mask;
      for (int i = 0; i < QF_STRIDE; i++)
        if (row[i] != skip) *p++ = row[i];
    } else if (t == 3) {
      mcpy(p, row, QF_STRIDE);
      p += QF_STRIDE;
    }
  }
  return (uint32_t)(p - out);
}

static int layer_same(const uint8_t *a, const uint8_t *b) {
  for (int i = 0; i < QF_LAYER_BYTES; i++)
    if (a[i] != (b ? b[i] : 0)) return 0;
  return 1;
}

uint32_t qf_frame_encode(const QfFrame *f, const QfFrame *prev, uint8_t *out) {
  uint8_t flags = (uint8_t)((prev ? 1 : 0) | (f->paper ? 2 : 0) | ((f->ink[0] & 3) << 2) | ((f->ink[1] & 3) << 4));
  uint32_t n = 1;
  for (int l = 0; l < 2; l++) {
    const uint8_t *pl = prev ? prev->px[l] : 0;
    if (layer_same(f->px[l], pl)) flags |= (uint8_t)(0x40 << l);
    else n += enc_layer(f->px[l], pl, out + n);
  }
  out[0] = flags;
  return n;
}

uint32_t qf_frame_decode(QfFrame *f, const uint8_t *data, uint32_t len) {
  if (len < 1) return 0;
  uint8_t flags = data[0];
  int rel = flags & 1;
  int ink0 = (flags >> 2) & 3, ink1 = (flags >> 4) & 3;
  if (ink0 > 2 || ink1 > 2) return 0;
  uint32_t p = 1;
  for (int l = 0; l < 2; l++) {
    uint8_t *dst = f->px[l];
    if (flags & (0x40 << l)) {
      if (!rel) mset(dst, 0, QF_LAYER_BYTES);
      continue;
    }
    if (len - p < 48) return 0;
    const uint8_t *types = data + p;
    p += 48;
    for (int y = 0; y < QF_H; y++) {
      int t = (types[y >> 2] >> (6 - 2 * (y & 3))) & 3;
      uint8_t row[QF_STRIDE];
      if (t == 0) {
        mset(row, 0, QF_STRIDE);
      } else if (t == 3) {
        if (len - p < QF_STRIDE) return 0;
        mcpy(row, data + p, QF_STRIDE);
        p += QF_STRIDE;
      } else {
        if (len - p < 4) return 0;
        uint32_t mask = ((uint32_t)data[p] << 24) | ((uint32_t)data[p + 1] << 16) |
                        ((uint32_t)data[p + 2] << 8) | data[p + 3];
        p += 4;
        for (int i = 0; i < QF_STRIDE; i++) {
          if (mask & (0x80000000u >> i)) {
            if (p >= len) return 0;
            row[i] = data[p++];
          } else {
            row[i] = t == 1 ? 0 : 0xFF;
          }
        }
      }
      uint8_t *d = dst + y * QF_STRIDE;
      if (rel) for (int i = 0; i < QF_STRIDE; i++) d[i] ^= row[i];
      else mcpy(d, row, QF_STRIDE);
    }
  }
  f->paper = (flags >> 1) & 1;
  f->ink[0] = (uint8_t)ink0;
  f->ink[1] = (uint8_t)ink1;
  return p;
}

// ---------------------------------------------------------------- notes

void qf_note_init(QfNote *n) {
  mset(n, 0, sizeof *n);
  n->speed = 4;
  n->loop = 1;
}

void qf_note_free(QfNote *n) {
  for (int i = 0; i < n->count; i++) qf_free(n->frames[i].data);
  qf_free(n->frames);
  n->frames = 0;
  n->count = n->cap = 0;
}

int qf_note_get(const QfNote *n, int i, QfFrame *f) {
  if (i < 0 || i >= n->count) return -1;
  const QfStored *s = &n->frames[i];
  return qf_frame_decode(f, s->data, s->len) == s->len ? 0 : -1;
}

static int store(QfStored *s, const QfFrame *f, uint8_t *tmp) {
  uint32_t len = qf_frame_encode(f, 0, tmp);
  uint8_t *d = (uint8_t *)qf_malloc(len);
  if (!d) return -1;
  mcpy(d, tmp, len);
  s->data = d;
  s->len = len;
  return 0;
}

int qf_note_set(QfNote *n, int i, const QfFrame *f) {
  if (i < 0 || i >= n->count) return -1;
  uint8_t *tmp = (uint8_t *)qf_malloc(QF_MAX_FRAME_BYTES);
  if (!tmp) return -1;
  QfStored s;
  int r = store(&s, f, tmp);
  qf_free(tmp);
  if (r) return -1;
  qf_free(n->frames[i].data);
  n->frames[i] = s;
  return 0;
}

int qf_note_insert(QfNote *n, int i, const QfFrame *f) {
  if (i < 0 || i > n->count || n->count >= QF_MAX_FRAMES) return -1;
  if (n->count == n->cap) {
    int nc = n->cap ? n->cap * 2 : 16;
    if (nc > QF_MAX_FRAMES) nc = QF_MAX_FRAMES;
    QfStored *nf = (QfStored *)qf_realloc(n->frames, sizeof(QfStored) * nc);
    if (!nf) return -1;
    n->frames = nf;
    n->cap = (uint16_t)nc;
  }
  uint8_t *tmp = (uint8_t *)qf_malloc(QF_MAX_FRAME_BYTES);
  if (!tmp) return -1;
  QfStored s;
  int r = store(&s, f, tmp);
  qf_free(tmp);
  if (r) return -1;
  for (int k = n->count; k > i; k--) n->frames[k] = n->frames[k - 1];
  n->frames[i] = s;
  n->count++;
  return 0;
}

int qf_note_delete(QfNote *n, int i) {
  if (i < 0 || i >= n->count) return -1;
  qf_free(n->frames[i].data);
  for (int k = i; k < n->count - 1; k++) n->frames[k] = n->frames[k + 1];
  n->count--;
  if (n->thumb >= n->count) n->thumb = n->count ? (uint16_t)(n->count - 1) : 0;
  return 0;
}

int qf_note_move(QfNote *n, int from, int to) {
  if (from < 0 || from >= n->count || to < 0 || to >= n->count) return -1;
  QfStored s = n->frames[from];
  if (from < to) for (int k = from; k < to; k++) n->frames[k] = n->frames[k + 1];
  else for (int k = from; k > to; k--) n->frames[k] = n->frames[k - 1];
  n->frames[to] = s;
  return 0;
}

// ---------------------------------------------------------------- files
// "QFLP", version byte 1, then chunks: 4-byte tag, 4-byte little-endian length, data.
// META must come before FRMS; unknown chunks are skipped.

static uint32_t rd32(const uint8_t *p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int tag_is(const uint8_t *p, const char *t) {
  return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3];
}

// META: u16 frame count, u8 speed, u8 flags (bit 0 loop), u16 thumbnail frame, u32 created,
// 8-byte author id, u8 author length, author (UTF-8).
static int parse_meta(QfNote *n, const uint8_t *d, uint32_t len, uint16_t *count) {
  if (len < 19) return -3;
  *count = (uint16_t)(d[0] | (d[1] << 8));
  n->speed = d[2];
  n->loop = d[3] & 1;
  n->thumb = (uint16_t)(d[4] | (d[5] << 8));
  n->created = rd32(d + 6);
  mcpy(n->author_id, d + 10, 8);
  uint32_t al = d[18];
  if (al > QF_MAX_AUTHOR || 19 + al > len) return -3;
  mcpy(n->author, d + 19, al);
  n->author[al] = 0;
  if (*count < 1 || *count > QF_MAX_FRAMES || n->speed < 1 || n->speed > 8) return -3;
  if (n->thumb >= *count) n->thumb = 0;
  return 0;
}

static int walk(QfNote *n, const uint8_t *data, uint32_t len, int frames) {
  if (len < 5 || !tag_is(data, "QFLP")) return -1;
  if (data[4] != 1) return -2;
  uint32_t p = 5;
  uint16_t count = 0;
  int have_meta = 0;
  while (len - p >= 8) {
    const uint8_t *tag = data + p;
    uint32_t cl = rd32(data + p + 4);
    p += 8;
    if (cl > len - p) return -4;
    const uint8_t *c = data + p;
    if (tag_is(tag, "META")) {
      int r = parse_meta(n, c, cl, &count);
      if (r) return r;
      have_meta = 1;
      if (!frames) return 0;
    } else if (tag_is(tag, "FRMS") && frames) {
      if (!have_meta) return -3;
      QfFrame *f = (QfFrame *)qf_malloc(sizeof(QfFrame));
      uint8_t *tmp = (uint8_t *)qf_malloc(QF_MAX_FRAME_BYTES);
      n->frames = (QfStored *)qf_malloc(sizeof(QfStored) * count);
      if (!f || !tmp || !n->frames) { qf_free(f); qf_free(tmp); return -5; }
      n->cap = count;
      qf_frame_clear(f);
      uint32_t q = 0;
      int err = 0;
      for (int i = 0; i < count && !err; i++) {
        if (cl - q < 2) { err = -4; break; }
        uint32_t fl = c[q] | (c[q + 1] << 8);
        q += 2;
        if (fl > cl - q) { err = -4; break; }
        if ((c[q] & 1) && i == 0) { err = -4; break; }
        if (qf_frame_decode(f, c + q, fl) != fl) { err = -4; break; }
        q += fl;
        if (store(&n->frames[i], f, tmp)) { err = -5; break; }
        n->count = (uint16_t)(i + 1);
      }
      qf_free(f);
      qf_free(tmp);
      return err;
    }
    p += cl;
  }
  return have_meta ? -4 : -3;
}

int qf_note_peek(QfNote *hdr, const uint8_t *data, uint32_t len) {
  qf_note_init(hdr);
  return walk(hdr, data, len, 0);
}

int qf_note_load(QfNote *n, const uint8_t *data, uint32_t len) {
  qf_note_init(n);
  int r = walk(n, data, len, 1);
  if (r) { qf_note_free(n); qf_note_init(n); }
  return r;
}

uint32_t qf_note_save(const QfNote *n, uint8_t **out) {
  *out = 0;
  if (n->count < 1) return 0;
  uint32_t al = 0;
  while (al < QF_MAX_AUTHOR && n->author[al]) al++;
  uint32_t cap = 64 + al, len = 0;
  for (int i = 0; i < n->count; i++) cap += 2 + n->frames[i].len;
  uint8_t *b = (uint8_t *)qf_malloc(cap);
  QfFrame *cur = (QfFrame *)qf_malloc(sizeof(QfFrame) * 2);
  uint8_t *tmp = (uint8_t *)qf_malloc(QF_MAX_FRAME_BYTES);
  if (!b || !cur || !tmp) { qf_free(b); qf_free(cur); qf_free(tmp); return 0; }
  mcpy(b, "QFLP\1", 5);
  len = 5;
  mcpy(b + len, "META", 4);
  wr32(b + len + 4, 19 + al);
  uint8_t *m = b + len + 8;
  m[0] = (uint8_t)n->count; m[1] = (uint8_t)(n->count >> 8);
  m[2] = n->speed; m[3] = n->loop & 1;
  m[4] = (uint8_t)n->thumb; m[5] = (uint8_t)(n->thumb >> 8);
  wr32(m + 6, n->created);
  mcpy(m + 10, n->author_id, 8);
  m[18] = (uint8_t)al;
  mcpy(m + 19, n->author, al);
  len += 8 + 19 + al;
  uint32_t frms = len;
  mcpy(b + len, "FRMS", 4);
  len += 8;
  // Each frame is written relative to the one before when that is smaller. The stored copy is
  // already the stand-alone encoding, so only the relative one needs working out.
  QfFrame *prev = &cur[0], *f = &cur[1];
  for (int i = 0; i < n->count; i++) {
    const QfStored *s = &n->frames[i];
    if (qf_frame_decode(f, s->data, s->len) != s->len) goto fail;
    const uint8_t *src = s->data;
    uint32_t fl = s->len;
    if (i > 0) {
      uint32_t rl = qf_frame_encode(f, prev, tmp);
      if (rl < fl) { src = tmp; fl = rl; }
    }
    b[len] = (uint8_t)fl; b[len + 1] = (uint8_t)(fl >> 8);
    mcpy(b + len + 2, src, fl);
    len += 2 + fl;
    QfFrame *t = prev; prev = f; f = t;
  }
  wr32(b + frms + 4, len - frms - 8);
  qf_free(cur);
  qf_free(tmp);
  *out = b;
  return len;
fail:
  qf_free(b); qf_free(cur); qf_free(tmp);
  return 0;
}
