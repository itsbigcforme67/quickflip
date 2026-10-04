// QuickFlip on the Cheap Yellow Display: draw flipbook notes with the stylus, keep them on the SD
// card, and share them with the QuickFlip server over Wi-Fi.
//
// Screens: gallery (your notes on the SD card), editor, editor menu, online (notes on the
// server) and player (an online note playing). Settings come from /quickflip/config.json on the
// SD card (see config.example.json).
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <algorithm>
#include <vector>

#include "board.h"
#include "qflip.h"
#include "touch.h"

// Frames go in PSRAM when the board has it, otherwise in normal memory.
extern "C" {
void *qf_malloc(size_t n) { return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT); }
void *qf_realloc(void *p, size_t n) { return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT); }
void qf_free(void *p) { free(p); }
}

static const char *DIR = "/quickflip";
static const size_t MAX_FILE = 400 * 1024;
static const uint32_t KEEP_FREE = 40 * 1024;  // memory left over for Wi-Fi and the fill tool

static LGFX tft;
static Preferences prefs;
static bool sd_ok = false;

// ---------------------------------------------------------------- settings
struct Config {
  String ssid, pass, server, key, author;
} cfg;

static void load_config() {
  cfg.author = "CYD";
  if (!sd_ok) return;
  File f = SD.open(String(DIR) + "/config.json");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    cfg.ssid = doc["wifi_ssid"] | "";
    cfg.pass = doc["wifi_pass"] | "";
    cfg.server = doc["server"] | "";
    cfg.key = doc["key"] | "";
    cfg.author = doc["author"] | "CYD";
    while (cfg.server.endsWith("/")) cfg.server.remove(cfg.server.length() - 1);
  }
  f.close();
}

// ---------------------------------------------------------------- colours and drawing
static lgfx::rgb565_t pal[2][8];
static uint16_t C_BG, C_PANEL, C_INK, C_MUTED, C_LINE, C_ACCENT, C_ACCENT_INK, C_STAGE;

static void init_colours() {
  static const uint8_t base[4][3] = {{255, 255, 255}, {24, 22, 20}, {226, 44, 32}, {40, 80, 220}};
  for (int p = 0; p < 2; p++)
    for (int i = 0; i < 4; i++) {
      const uint8_t *c = base[i], *bg = base[p];
      pal[p][i] = lgfx::rgb565_t(c[0], c[1], c[2]);
      pal[p][i + 4] = lgfx::rgb565_t((c[0] * 28 + bg[0] * 72) / 100, (c[1] * 28 + bg[1] * 72) / 100, (c[2] * 28 + bg[2] * 72) / 100);
    }
  C_BG = tft.color565(251, 243, 223);
  C_PANEL = tft.color565(255, 250, 240);
  C_INK = tft.color565(43, 38, 32);
  C_MUTED = tft.color565(138, 127, 111);
  C_LINE = tft.color565(214, 196, 160);
  C_ACCENT = tft.color565(244, 177, 61);
  C_ACCENT_INK = tft.color565(74, 49, 4);
  C_STAGE = tft.color565(217, 204, 176);
}

static uint16_t ink_rgb(int paper, int ink) {
  if (ink == QF_INK_RED) return tft.color565(226, 44, 32);
  if (ink == QF_INK_BLUE) return tft.color565(40, 80, 220);
  return paper ? TFT_WHITE : tft.color565(24, 22, 20);
}

// Draw rows y0..y1 of a frame with its top left corner at (ox, oy).
static void draw_frame(const QfFrame *f, const QfFrame *onion, int ox, int oy, int y0 = 0, int y1 = QF_H - 1) {
  if (y0 < 0) y0 = 0;
  if (y1 > QF_H - 1) y1 = QF_H - 1;
  static uint8_t idx[QF_W];
  static lgfx::rgb565_t line[QF_W];
  const lgfx::rgb565_t *p = pal[f->paper ? 1 : 0];
  tft.startWrite();
  for (int y = y0; y <= y1; y++) {
    qf_render_row(f, onion, y, idx);
    for (int x = 0; x < QF_W; x++) line[x] = p[idx[x]];
    tft.pushImage(ox, oy + y, QF_W, 1, line);
  }
  tft.endWrite();
}

static void draw_thumb(const uint8_t *px, int ox, int oy) {
  static lgfx::rgb565_t line[64];
  tft.startWrite();
  for (int y = 0; y < 48; y++) {
    for (int x = 0; x < 64; x++) line[x] = pal[0][px[y * 64 + x] & 7];
    tft.pushImage(ox, oy + y, 64, 1, line);
  }
  tft.endWrite();
}

static void button(int x, int y, int w, int h, const char *label, bool on = false, bool enabled = true) {
  tft.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 6, on ? C_ACCENT : C_PANEL);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, on ? C_ACCENT_INK : C_LINE);
  tft.setTextColor(enabled ? (on ? C_ACCENT_INK : C_INK) : C_LINE);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::Font2);
  tft.drawString(label, x + w / 2, y + h / 2);
}

static bool in(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && y >= by && x < bx + bw && y < by + bh; }

// A box in the middle of the screen. With wait, it stays until tapped.
static void wait_release();
static void message(const String &text, bool wait = false) {
  tft.fillRoundRect(20, 70, 280, 100, 10, C_PANEL);
  tft.drawRoundRect(20, 70, 280, 100, 10, C_ACCENT_INK);
  tft.setTextColor(C_INK);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_center);
  // up to three lines, split at spaces
  String rest = text;
  int line = 0;
  String lines[3];
  while (rest.length() && line < 3) {
    int cut = rest.length();
    if (tft.textWidth(rest) > 260) {
      cut = 0;
      for (int i = 0; i < (int)rest.length(); i++)
        if (rest[i] == ' ' && tft.textWidth(rest.substring(0, i)) <= 260) cut = i;
      if (!cut) cut = rest.length();
    }
    lines[line++] = rest.substring(0, cut);
    rest = rest.substring(cut);
    rest.trim();
  }
  int y = 120 - (line - 1) * 9 - (wait ? 8 : 0);
  for (int i = 0; i < line; i++) tft.drawString(lines[i], 160, y + i * 18);
  if (wait) {
    tft.setTextColor(C_MUTED);
    tft.drawString("tap to go on", 160, 155);
    int x, yy;
    while (!touch_read(&x, &yy)) delay(10);
    wait_release();
  }
}

static bool confirm(const String &question, const char *yes) {
  tft.fillRoundRect(20, 60, 280, 120, 10, C_PANEL);
  tft.drawRoundRect(20, 60, 280, 120, 10, C_ACCENT_INK);
  tft.setTextColor(C_INK);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_center);
  tft.drawString(question, 160, 95);
  button(40, 120, 110, 44, "Cancel");
  button(170, 120, 110, 44, yes, true);
  int x, y;
  for (;;) {
    while (!touch_read(&x, &y)) delay(10);
    wait_release();
    if (in(x, y, 40, 120, 110, 44)) return false;
    if (in(x, y, 170, 120, 110, 44)) return true;
  }
}

static void wait_release() {
  int x, y, up = 0;
  while (up < 3) {
    up = touch_read(&x, &y) ? 0 : up + 1;
    delay(10);
  }
}

// ---------------------------------------------------------------- touch calibration
static void calibrate() {
  static const int pts[3][2] = {{20, 20}, {300, 20}, {20, 220}};
  int raw[3][2];
  for (int i = 0; i < 3; i++) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_INK);
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(middle_center);
    tft.drawString("Touch screen setup", 160, 100);
    tft.drawString("Press the middle of the cross with the stylus", 160, 125);
    int cx = pts[i][0], cy = pts[i][1];
    tft.drawLine(cx - 12, cy, cx + 12, cy, TFT_RED);
    tft.drawLine(cx, cy - 12, cx, cy + 12, TFT_RED);
    int rx, ry;
    long sx = 0, sy = 0;
    int n = 0;
    while (!touch_raw(&rx, &ry)) delay(10);
    delay(80);
    while (n < 16) {
      if (touch_raw(&rx, &ry)) { sx += rx; sy += ry; n++; }
      delay(10);
    }
    raw[i][0] = sx / n;
    raw[i][1] = sy / n;
    wait_release();
  }
  // moving right on the screen: whichever raw value changed most is the screen's X
  int dx = abs(raw[1][0] - raw[0][0]), dy = abs(raw[1][1] - raw[0][1]);
  TouchCal c;
  c.swap = dy > dx;
  int ax = c.swap ? 1 : 0, ay = c.swap ? 0 : 1;
  float kx = (raw[1][ax] - raw[0][ax]) / 280.0f;  // raw per screen pixel
  float ky = (raw[2][ay] - raw[0][ay]) / 200.0f;
  c.x0 = raw[0][ax] - 20 * kx;
  c.x1 = c.x0 + 320 * kx;
  c.y0 = raw[0][ay] - 20 * ky;
  c.y1 = c.y0 + 240 * ky;
  c.valid = 1;
  touch_cal = c;
  prefs.putBytes("cal", &c, sizeof c);
}

// ---------------------------------------------------------------- the note
static QfNote note;
static QfFrame *cur, *onion_f, *undo_f, *spare;  // spare: playback and new frames
static int idx = 0;
static bool dirty = false, note_dirty = false, has_undo = false;
static String note_path;   // empty for a note not saved yet
static String online_id;   // the note came from the server

static void author_id(uint8_t out[8]) {
  uint64_t mac = ESP.getEfuseMac();
  for (int i = 0; i < 6; i++) out[i] = (mac >> (8 * i)) & 0xFF;
  out[6] = 'Q';
  out[7] = 'F';
}

static bool commit() {
  if (!dirty) return true;
  if (qf_note_set(&note, idx, cur)) {
    message("Out of memory: this frame could not be kept. Save the note.", true);
    return false;
  }
  dirty = false;
  note_dirty = true;
  return true;
}

static void load_frame(int i) {
  idx = i < 0 ? 0 : i >= note.count ? note.count - 1 : i;
  qf_note_get(&note, idx, cur);
  if (idx > 0) qf_note_get(&note, idx - 1, onion_f);
  has_undo = false;
}

static void new_note() {
  qf_note_free(&note);
  qf_note_init(&note);
  qf_frame_clear(cur);
  qf_note_insert(&note, 0, cur);
  note_path = "";
  online_id = "";
  dirty = note_dirty = false;
  load_frame(0);
}

static bool load_file(const String &path) {
  File f = SD.open(path);
  if (!f) return false;
  size_t n = f.size();
  if (n > MAX_FILE) { f.close(); return false; }
  uint8_t *buf = (uint8_t *)qf_malloc(n);
  if (!buf) { f.close(); return false; }
  bool ok = f.read(buf, n) == n;
  f.close();
  qf_note_free(&note);
  ok = ok && qf_note_load(&note, buf, n) == 0;
  qf_free(buf);
  if (!ok) { new_note(); return false; }
  note_path = path;
  online_id = "";
  dirty = note_dirty = false;
  load_frame(0);
  return true;
}

static String next_path() {
  int n = prefs.getInt("next", 1);
  String p;
  do {
    char name[40];
    snprintf(name, sizeof name, "%s/qf%05d.qflip", DIR, n++);
    p = name;
  } while (SD.exists(p));
  prefs.putInt("next", n);
  return p;
}

static String thumb_path(const String &p) { return p.substring(0, p.length() - 6) + ".thm"; }

// Save to the SD card. Returns false (after saying why) if it could not.
static bool save_note() {
  if (!commit()) return false;
  if (!sd_ok) { message("No SD card, so the note cannot be saved.", true); return false; }
  author_id(note.author_id);
  if (online_id.isEmpty()) {
    // keep the original author on notes from the server
    strncpy(note.author, cfg.author.c_str(), QF_MAX_AUTHOR);
    note.author[QF_MAX_AUTHOR] = 0;
  }
  time_t now = time(nullptr);
  if (!note.created && now > 1700000000) note.created = (uint32_t)now;
  uint8_t *data = nullptr;
  uint32_t n = qf_note_save(&note, &data);
  if (!n) { message("Out of memory while saving.", true); return false; }
  if (note_path.isEmpty()) note_path = next_path();
  File f = SD.open(note_path, FILE_WRITE);
  bool ok = f && f.write(data, n) == n;
  if (f) f.close();
  qf_free(data);
  if (!ok) { message("Could not write to the SD card.", true); return false; }
  // a 64x48 picture for the gallery
  if (qf_note_get(&note, note.thumb < note.count ? note.thumb : 0, spare) == 0) {
    static uint8_t th[64 * 48];
    qf_render_thumb(spare, th);
    File t = SD.open(thumb_path(note_path), FILE_WRITE);
    if (t) { t.write(th, sizeof th); t.close(); }
  }
  note_dirty = false;
  online_id = "";
  return true;
}

// ---------------------------------------------------------------- Wi-Fi and the server
static bool wifi_up() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (cfg.ssid.isEmpty()) {
    message("No Wi-Fi set up. Put wifi_ssid and wifi_pass in /quickflip/config.json on the SD card.", true);
    return false;
  }
  message("Connecting to " + cfg.ssid + "...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    message("Could not join the Wi-Fi network " + cfg.ssid + ".", true);
    return false;
  }
  configTime(0, 0, "pool.ntp.org");
  return true;
}

static bool server_ready() {
  if (cfg.server.isEmpty()) {
    message("No server set up. Put \"server\" in /quickflip/config.json on the SD card.", true);
    return false;
  }
  return wifi_up();
}

// GET into memory from qf_malloc. Returns the length, or -1.
static int http_get(const String &url, uint8_t **out, size_t max) {
  *out = nullptr;
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) return -1;
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len <= 0 || (size_t)len > max) { http.end(); return -1; }
  uint8_t *buf = (uint8_t *)qf_malloc(len);
  if (!buf) { http.end(); return -1; }
  WiFiClient *s = http.getStreamPtr();
  int got = 0;
  uint32_t t0 = millis();
  while (got < len && millis() - t0 < 15000) {
    int a = s->available();
    if (a > 0) got += s->readBytes(buf + got, min(a, len - got));
    else delay(2);
  }
  http.end();
  if (got != len) { qf_free(buf); return -1; }
  *out = buf;
  return len;
}

static void upload_note() {
  if (!save_note() || !server_ready()) return;
  if (cfg.key.isEmpty()) { message("No upload key set up. Put \"key\" in /quickflip/config.json.", true); return; }
  File f = SD.open(note_path);
  size_t n = f ? f.size() : 0;
  uint8_t *buf = n ? (uint8_t *)qf_malloc(n) : nullptr;
  if (!buf) { if (f) f.close(); message("Out of memory.", true); return; }
  f.read(buf, n);
  f.close();
  message("Uploading...");
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(cfg.server + "/api/notes");
  http.addHeader("Authorization", "Bearer " + cfg.key);
  http.addHeader("Content-Type", "application/octet-stream");
  int code = http.POST(buf, n);
  String body = http.getString();
  http.end();
  qf_free(buf);
  JsonDocument doc;
  deserializeJson(doc, body);
  if (code == 201) {
    String id = doc["id"] | "";
    message("Uploaded! " + cfg.server + "/n/" + id, true);
  } else {
    String err = doc["error"].is<const char *>() ? String((const char *)doc["error"]) : "error " + String(code);
    message("Upload failed: " + err, true);
  }
}

// ---------------------------------------------------------------- screens
enum Screen { S_GALLERY, S_EDIT, S_MENU, S_ONLINE, S_PLAYER };
static Screen screen;
static void show(Screen s);

// ---- editor
enum Tool { T_PEN, T_ERASER, T_FILL };
static Tool tool = T_PEN;
static const uint8_t SIZES[] = {1, 2, 3, 5, 8};
static int size_i = 1, pat = 0, layer = 0;
static bool onion_on = true;
static bool stroking = false;
static int last_x, last_y;
static bool playing = false;
static int play_i;
static uint32_t play_next;
static const char *PAT_NAMES[] = {"Solid", "Half", "Light", "Lines"};
static const char *INK_NAMES[] = {"Auto", "Red", "Blue"};

static void draw_canvas(int y0 = 0, int y1 = QF_H - 1) {
  draw_frame(cur, onion_on && idx > 0 ? onion_f : nullptr, 0, 0, y0, y1);
}

static void draw_edit_bar() {
  const char *tools[] = {"Pen", "Erase", "Fill"};
  for (int i = 0; i < 3; i++) button(256, i * 48, 64, 48, tools[i], tool == i);
  char s[12];
  snprintf(s, sizeof s, "Size %d", SIZES[size_i]);
  button(256, 144, 64, 48, s);
  button(256, 192, 64, 48, "Undo", false, has_undo);
  tft.fillRect(0, 192, 256, 48, C_BG);
  button(0, 192, 42, 48, "Menu");
  button(42, 192, 42, 48, layer ? "B" : "A", true);
  tft.fillCircle(52, 204, 4, ink_rgb(cur->paper, cur->ink[layer]));
  tft.drawCircle(52, 204, 5, C_ACCENT_INK);
  button(84, 192, 42, 48, "<", false, idx > 0);
  char pos[16];
  snprintf(pos, sizeof pos, "%d/%d", idx + 1, note.count);
  tft.setTextColor(C_INK);
  tft.setTextDatum(middle_center);
  tft.drawString(pos, 149, 216);
  button(172, 192, 42, 48, idx < note.count - 1 ? ">" : "+");
  button(214, 192, 42, 48, playing ? "Stop" : "Play");
}

static void push_undo() {
  memcpy(undo_f, cur, sizeof(QfFrame));
  if (!has_undo) { has_undo = true; draw_edit_bar(); }
}

static void stop_playing() {
  playing = false;
  if (screen == S_EDIT) { draw_canvas(); draw_edit_bar(); }
}

static void play_tick(int ox) {
  if (!playing || millis() < play_next) return;
  if (play_i >= note.count) {
    if (!note.loop && screen == S_EDIT) { stop_playing(); return; }
    play_i = 0;
  }
  qf_note_get(&note, play_i, spare);
  draw_frame(spare, nullptr, ox, 0);
  play_i++;
  play_next += 2000 / qf_speed_fps2[note.speed];
  if ((int32_t)(millis() - play_next) > 500) play_next = millis();  // fell far behind
}

static void go_frame(int i) {
  if (!commit()) return;
  load_frame(i);
  draw_canvas();
  draw_edit_bar();
}

static bool room_for_frame() {
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < KEEP_FREE + QF_MAX_FRAME_BYTES) {
    message("Memory is full: no more frames fit. Save, then start a new note.", true);
    return false;
  }
  return true;
}

static void edit_press(int x, int y) {
  if (playing) { wait_release(); stop_playing(); return; }
  if (x < QF_W && y < QF_H) {
    if (tool == T_FILL) {
      push_undo();
      if (qf_fill(cur, layer, x, y, pat, QF_DRAW)) message("Out of memory", true);
      dirty = true;
      draw_canvas();
      wait_release();
      return;
    }
    push_undo();
    stroking = true;
    last_x = x;
    last_y = y;
    int sz = SIZES[size_i];
    qf_dot(cur, layer, x, y, sz, tool == T_ERASER ? 0 : pat, tool == T_ERASER ? QF_ERASE : QF_DRAW);
    dirty = true;
    draw_canvas(y - sz, y + sz);
    return;
  }
  wait_release();
  if (x >= QF_W) {
    int b = y / 48;
    if (b < 3) tool = (Tool)b;
    else if (b == 3) size_i = (size_i + 1) % (int)sizeof SIZES;
    else if (has_undo) {
      memcpy(cur, undo_f, sizeof(QfFrame));
      has_undo = false;
      dirty = true;
      draw_canvas();
    }
    draw_edit_bar();
    return;
  }
  if (x < 42) { show(S_MENU); return; }
  if (x < 84) { layer ^= 1; draw_edit_bar(); return; }
  if (x < 126) { if (idx > 0) go_frame(idx - 1); return; }
  if (x < 172) return;
  if (x < 214) {
    if (idx < note.count - 1) { go_frame(idx + 1); return; }
    if (note.count >= QF_MAX_FRAMES || !commit() || !room_for_frame()) return;
    qf_frame_clear(spare);  // a blank frame with the same paper and inks
    spare->paper = cur->paper;
    spare->ink[0] = cur->ink[0];
    spare->ink[1] = cur->ink[1];
    if (qf_note_insert(&note, idx + 1, spare)) { message("Out of memory", true); return; }
    note_dirty = true;
    go_frame(idx + 1);
    return;
  }
  if (!commit()) return;
  playing = true;
  play_i = 0;
  play_next = millis();
  draw_edit_bar();
}

static void edit_drag(int x, int y) {
  if (!stroking) return;
  x = constrain(x, 0, QF_W - 1);
  y = constrain(y, 0, QF_H - 1);
  if (x == last_x && y == last_y) return;
  int sz = SIZES[size_i];
  qf_line(cur, layer, last_x, last_y, x, y, sz, tool == T_ERASER ? 0 : pat, tool == T_ERASER ? QF_ERASE : QF_DRAW);
  draw_canvas(min(y, last_y) - sz, max(y, last_y) + sz);
  last_x = x;
  last_y = y;
}

// ---- editor menu: 3 x 4 buttons
static String menu_label(int i) {
  char s[32];
  switch (i) {
    case 0: snprintf(s, sizeof s, "Ink A: %s", INK_NAMES[cur->ink[0]]); break;
    case 1: snprintf(s, sizeof s, "Ink B: %s", INK_NAMES[cur->ink[1]]); break;
    case 2: snprintf(s, sizeof s, "Paper: %s", cur->paper ? "Black" : "White"); break;
    case 3: snprintf(s, sizeof s, "Pattern: %s", PAT_NAMES[pat]); break;
    case 4: snprintf(s, sizeof s, "Onion: %s", onion_on ? "On" : "Off"); break;
    case 5: snprintf(s, sizeof s, "Speed: %d", note.speed); break;
    case 6: return "Copy frame";
    case 7: return "Delete frame";
    case 8: snprintf(s, sizeof s, "Loop: %s", note.loop ? "On" : "Off"); break;
    case 9: return "Save + exit";
    case 10: return "Upload";
    default: return "Back";
  }
  return s;
}

static void draw_menu() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_INK);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(middle_left);
  char head[64];
  int kb = 0;
  for (int i = 0; i < note.count; i++) kb += note.frames[i].len;
  snprintf(head, sizeof head, "Frame %d of %d   %d KB   %u KB free", idx + 1, note.count, (kb + 1023) / 1024,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
  tft.drawString(head, 8, 18);
  for (int i = 0; i < 12; i++) button((i % 3) * 106 + 1, 36 + (i / 3) * 51, 106, 51, menu_label(i).c_str(), i == 11);
}

static void menu_press(int x, int y) {
  wait_release();
  if (y < 36) return;
  int i = ((y - 36) / 51) * 3 + min(x / 106, 2);
  switch (i) {
    case 0: case 1: push_undo(); cur->ink[i] = (cur->ink[i] + 1) % 3; dirty = true; break;
    case 2: push_undo(); cur->paper ^= 1; dirty = true; break;
    case 3: pat = (pat + 1) % QF_PAT_COUNT; break;
    case 4: onion_on = !onion_on; break;
    case 5: note.speed = note.speed % 8 + 1; note_dirty = true; break;
    case 6:
      if (note.count >= QF_MAX_FRAMES || !commit() || !room_for_frame()) break;
      if (qf_note_insert(&note, idx + 1, cur)) { message("Out of memory", true); break; }
      note_dirty = true;
      load_frame(idx + 1);
      show(S_EDIT);
      return;
    case 7:
      if (note.count == 1) {
        push_undo();
        uint8_t p = cur->paper, a = cur->ink[0], b = cur->ink[1];
        qf_frame_clear(cur);
        cur->paper = p; cur->ink[0] = a; cur->ink[1] = b;
        dirty = true;
      } else {
        if (!confirm("Delete this frame?", "Delete")) break;
        qf_note_delete(&note, idx);
        dirty = false;
        note_dirty = true;
        load_frame(idx);
      }
      show(S_EDIT);
      return;
    case 8: note.loop ^= 1; note_dirty = true; break;
    case 9:
      if (save_note()) show(S_GALLERY);
      else draw_menu();
      return;
    case 10: upload_note(); break;
    default: show(S_EDIT); return;
  }
  draw_menu();
}

// ---- gallery: 4 x 3 tiles of 80 x 70 under a 28 pixel bar
static std::vector<String> files;
static int gal_page = 0;

static void list_files() {
  files.clear();
  if (!sd_ok) return;
  File d = SD.open(DIR);
  if (!d) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String n = f.name();
    if (n.endsWith(".qflip")) files.push_back(String(DIR) + "/" + (n.startsWith("/") ? n.substring(n.lastIndexOf('/') + 1) : n));
    f.close();
  }
  d.close();
  std::sort(files.begin(), files.end(), [](const String &a, const String &b) { return a > b; });
}

static void tile_box(int t, int *x, int *y) {
  *x = (t % 4) * 80;
  *y = 28 + (t / 4) * 70;
}

static void draw_gallery() {
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(middle_left);
  tft.setTextColor(C_ACCENT_INK);
  tft.drawString("Quick", 6, 14);
  tft.setTextColor(C_INK);
  tft.drawString("Flip", 6 + tft.textWidth("Quick"), 14);
  int pages = (files.size() + 1 + 11) / 12;
  button(170, 0, 36, 28, "<", false, gal_page > 0);
  button(206, 0, 36, 28, ">", false, gal_page < pages - 1);
  button(244, 0, 76, 28, "Online");
  if (!sd_ok) {
    message("No SD card found. Notes cannot be saved, but you can still draw.");
  }
  for (int t = 0; t < 12; t++) {
    int n = gal_page * 12 + t - 1;  // tile 0 of page 0 is "New"
    int x, y;
    tile_box(t, &x, &y);
    if (n == -1) {
      tft.fillRoundRect(x + 4, y + 2, 72, 66, 6, C_ACCENT);
      tft.setFont(&fonts::Font2);
      tft.setTextDatum(middle_center);
      tft.setTextColor(C_ACCENT_INK);
      tft.drawString("+ New", x + 40, y + 35);
      continue;
    }
    if (n < 0 || n >= (int)files.size()) continue;
    static uint8_t th[64 * 48];
    File f = SD.open(thumb_path(files[n]));
    bool have = f && f.read(th, sizeof th) == sizeof th;
    if (f) f.close();
    tft.drawRect(x + 7, y + 3, 66, 50, C_LINE);
    if (have) draw_thumb(th, x + 8, y + 4);
    else tft.fillRect(x + 8, y + 4, 64, 48, TFT_WHITE);
    // frame count from the file header
    uint8_t head[64];
    File g = SD.open(files[n]);
    int len = g ? g.read(head, sizeof head) : 0;
    if (g) g.close();
    QfNote h;
    char cap[24] = "?";
    if (len > 0 && qf_note_peek(&h, head, len) == 0) snprintf(cap, sizeof cap, "%d frames", h.count);
    tft.setFont(&fonts::Font0);
    tft.setTextDatum(middle_center);
    tft.setTextColor(C_MUTED);
    tft.drawString(cap, x + 40, y + 61);
  }
}

static uint32_t press_at;
static int press_tile = -1;

static void gallery_press(int x, int y) {
  press_at = millis();
  press_tile = -1;
  if (y < 28) {
    wait_release();
    int pages = (files.size() + 1 + 11) / 12;
    if (in(x, y, 170, 0, 36, 28) && gal_page > 0) { gal_page--; draw_gallery(); }
    else if (in(x, y, 206, 0, 36, 28) && gal_page < pages - 1) { gal_page++; draw_gallery(); }
    else if (x >= 244) show(S_ONLINE);
    return;
  }
  press_tile = (x / 80) + ((y - 28) / 70) * 4;
}

static void gallery_release() {
  if (press_tile < 0) return;
  int n = gal_page * 12 + press_tile - 1;
  bool held = millis() - press_at > 800;
  press_tile = -1;
  if (n == -1) { new_note(); show(S_EDIT); return; }
  if (n < 0 || n >= (int)files.size()) return;
  if (held) {
    if (confirm("Delete this note?", "Delete")) {
      SD.remove(files[n]);
      SD.remove(thumb_path(files[n]));
      list_files();
    }
    draw_gallery();
    return;
  }
  message("Opening...");
  if (!load_file(files[n])) { message("That note could not be opened (damaged, or too big).", true); draw_gallery(); return; }
  show(S_EDIT);
}

// ---- online: notes on the server, same tile layout
struct OnlineNote { String id, author; int stars; };
static std::vector<OnlineNote> online;
static int online_offset = 0, online_total = 0;
static bool online_top = false;

static void draw_online() {
  tft.fillScreen(C_BG);
  button(0, 0, 60, 28, "Back");
  button(64, 0, 70, 28, online_top ? "Top" : "New", true);
  button(170, 0, 36, 28, "<", false, online_offset > 0);
  button(206, 0, 36, 28, ">", false, online_offset + 12 < online_total);
  if (!server_ready()) { show(S_GALLERY); return; }
  message("Loading...");
  online.clear();
  uint8_t *body = nullptr;
  String url = cfg.server + "/api/notes?limit=12&offset=" + online_offset + "&sort=" + (online_top ? "top" : "new");
  int n = http_get(url, &body, 32 * 1024);
  if (n < 0) { message("Could not reach the server at " + cfg.server, true); show(S_GALLERY); return; }
  JsonDocument doc;
  deserializeJson(doc, body, n);
  qf_free(body);
  online_total = doc["total"] | 0;
  for (JsonObject o : doc["notes"].as<JsonArray>())
    online.push_back({o["id"] | "", o["author"] | "", o["stars"] | 0});
  tft.fillRect(0, 28, 320, 212, C_BG);
  button(170, 0, 36, 28, "<", false, online_offset > 0);
  button(206, 0, 36, 28, ">", false, online_offset + 12 < online_total);
  if (online.empty()) message("Nothing on the server yet.");
  for (size_t t = 0; t < online.size(); t++) {
    int x, y;
    tile_box(t, &x, &y);
    tft.drawRect(x + 7, y + 3, 66, 50, C_LINE);
    uint8_t *png = nullptr;
    int pl = http_get(cfg.server + "/api/notes/" + online[t].id + ".png", &png, 64 * 1024);
    if (pl > 0) tft.drawPng(png, pl, x + 8, y + 4, 64, 48, 0, 0, 0.25f, 0.25f);
    else tft.fillRect(x + 8, y + 4, 64, 48, TFT_WHITE);
    qf_free(png);
    String cap = online[t].author.substring(0, 9) + " *" + online[t].stars;
    tft.setFont(&fonts::Font0);
    tft.setTextDatum(middle_center);
    tft.setTextColor(C_MUTED);
    tft.drawString(cap, x + 40, y + 61);
  }
}

static void online_press(int x, int y) {
  wait_release();
  if (y < 28) {
    if (x < 60) show(S_GALLERY);
    else if (in(x, y, 64, 0, 70, 28)) { online_top = !online_top; online_offset = 0; draw_online(); }
    else if (in(x, y, 170, 0, 36, 28) && online_offset > 0) { online_offset -= 12; draw_online(); }
    else if (in(x, y, 206, 0, 36, 28) && online_offset + 12 < online_total) { online_offset += 12; draw_online(); }
    return;
  }
  size_t t = (x / 80) + ((y - 28) / 70) * 4;
  if (t >= online.size()) return;
  message("Downloading...");
  uint8_t *data = nullptr;
  int n = http_get(cfg.server + "/api/notes/" + online[t].id + ".qflip", &data, MAX_FILE);
  qf_note_free(&note);
  bool ok = n > 0 && qf_note_load(&note, data, n) == 0;
  qf_free(data);
  if (!ok) { new_note(); message("That note could not be downloaded (too big for this device?).", true); draw_online(); return; }
  note_path = "";
  online_id = online[t].id;
  dirty = note_dirty = false;
  load_frame(0);
  show(S_PLAYER);
}

// ---- player: an online note, canvas centred, buttons below
static void draw_player_bar() {
  tft.fillRect(0, 192, 320, 48, C_BG);
  button(0, 192, 80, 48, "Back");
  button(80, 192, 80, 48, playing ? "Pause" : "Play", true);
  button(160, 192, 80, 48, "Star");
  button(240, 192, 80, 48, "Keep");
}

static void player_press(int x, int y) {
  wait_release();
  if (y < 192) return;
  if (x < 80) { playing = false; show(S_ONLINE); return; }
  if (x < 160) {
    playing = !playing;
    play_next = millis();
    if (playing && play_i >= note.count) play_i = 0;
    draw_player_bar();
    return;
  }
  bool was = playing;
  playing = false;
  if (x < 240) {
    HTTPClient http;
    http.begin(cfg.server + "/api/notes/" + online_id + "/star");
    if (!cfg.key.isEmpty()) http.addHeader("Authorization", "Bearer " + cfg.key);
    int code = http.POST((uint8_t *)"", 0);
    http.end();
    message(code == 200 ? "Star given!" : "Could not reach the server.", true);
  } else {
    String keep = online_id;
    if (save_note()) message("Kept in your notes.", true);
    online_id = keep;
  }
  tft.fillScreen(C_BG);
  play_i = play_i > 0 ? play_i - 1 : 0;
  qf_note_get(&note, play_i, spare);
  draw_frame(spare, nullptr, 32, 0);
  playing = was;
  play_next = millis();
  draw_player_bar();
}

// ---------------------------------------------------------------- screen switching and the loop
static void show(Screen s) {
  screen = s;
  playing = false;
  stroking = false;
  switch (s) {
    case S_GALLERY:
      list_files();
      if (gal_page * 12 > (int)files.size()) gal_page = 0;
      draw_gallery();
      break;
    case S_EDIT:
      tft.fillScreen(C_BG);
      draw_canvas();
      draw_edit_bar();
      break;
    case S_MENU: draw_menu(); break;
    case S_ONLINE: draw_online(); break;
    case S_PLAYER:
      tft.fillScreen(C_BG);
      play_i = 0;
      playing = true;
      play_next = millis();
      draw_player_bar();
      break;
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(200);
  init_colours();
  touch_begin();
  prefs.begin("quickflip");

  cur = (QfFrame *)qf_malloc(sizeof(QfFrame));
  onion_f = (QfFrame *)qf_malloc(sizeof(QfFrame));
  undo_f = (QfFrame *)qf_malloc(sizeof(QfFrame));
  spare = (QfFrame *)qf_malloc(sizeof(QfFrame));
  qf_note_init(&note);

  // splash; holding the screen during it redoes the touch setup
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(middle_center);
  tft.setTextColor(C_INK);
  tft.drawString("QuickFlip", 160, 100);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(C_MUTED);
  tft.drawString("Hold the screen now to set up touch again", 160, 140);
  bool recal = false;
  int rx, ry;
  for (int i = 0; i < 150; i++) {
    if (touch_raw(&rx, &ry)) recal = true;
    delay(10);
  }
  TouchCal c;
  if (prefs.getBytes("cal", &c, sizeof c) == sizeof c && c.valid) touch_cal = c;
  if (recal) wait_release();
  if (recal || !touch_cal.valid) calibrate();

  sd_ok = SD.begin(QF_SD_CS);
  if (sd_ok && !SD.exists(DIR)) SD.mkdir(DIR);
  load_config();
  Serial.printf("QuickFlip: SD %s, %u KB free\n", sd_ok ? "ok" : "missing", (unsigned)(ESP.getFreeHeap() / 1024));
  new_note();
  show(S_GALLERY);
}

void loop() {
  static bool was_down = false;
  static int up_count = 0;
  int x, y;
  bool down = touch_read(&x, &y);
  if (down) {
    up_count = 0;
    if (!was_down) {
      was_down = true;
      switch (screen) {
        case S_GALLERY: gallery_press(x, y); break;
        case S_EDIT: edit_press(x, y); break;
        case S_MENU: menu_press(x, y); break;
        case S_ONLINE: online_press(x, y); break;
        case S_PLAYER: player_press(x, y); break;
      }
      // the handlers that wait for the stylus to lift leave it up
      if (screen != S_EDIT || !stroking) was_down = screen == S_GALLERY && press_tile >= 0;
    } else if (screen == S_EDIT) {
      edit_drag(x, y);
    }
  } else if (was_down && ++up_count >= 3) {  // a resistive screen flickers; wait for a steady lift
    was_down = false;
    if (screen == S_EDIT && stroking) { stroking = false; draw_edit_bar(); }
    if (screen == S_GALLERY) gallery_release();
  }
  if (screen == S_EDIT) play_tick(0);
  if (screen == S_PLAYER) play_tick(32);
  delay(2);
}
