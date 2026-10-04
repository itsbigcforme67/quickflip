// QuickFlip web app: draw flipbook notes, keep them in the browser, share them on a QuickFlip server.
// The drawing, frame storage and file format all live in core/qflip.wasm (built from core/*.c,
// the same code the ESP32 runs).
'use strict';

const W = 256, H = 192;
const PAPER_OFF = W * H / 8 * 2, INK_OFF = PAPER_OFF + 1;  // field offsets inside a QfFrame
const NOTE = { speed: 8, loop: 9, thumb: 10, created: 12, authorId: 16, author: 24 };  // inside QfNote
const SPEED_FPS = [0, 0.5, 1, 2, 4, 6, 12, 20, 30];
const SIZES = [1, 2, 3, 5, 8, 12];
const MAX_FRAMES = 999;
const DRAW = 0, ERASE = 1;
const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------- core
let api;
const mem = () => new Uint8Array(api.memory.buffer);
const slot = (s) => new Uint8Array(api.memory.buffer, api.api_slot(s), api.api_slot_size());
const noteView = () => new DataView(api.memory.buffer, api.api_note(), 64);

function loadBytes(bytes) {
  const p = api.api_alloc(bytes.length);
  if (!p) return -5;
  mem().set(bytes, p);
  const r = api.api_load(p, bytes.length);
  api.api_release(p);
  return r;
}
function saveBytes() {
  const n = api.api_save();
  if (!n) return null;
  const p = api.api_saved();
  return mem().slice(p, p + n);
}

// ---------------------------------------------------------------- colours and drawing
const BASE = [[255, 255, 255], [24, 22, 20], [226, 44, 32], [40, 80, 220]];
const palettes = [0, 1].map((paper) => {
  const bg = BASE[paper];
  const pal = new Uint32Array(8);
  const rgba = (c) => (255 << 24) | (c[2] << 16) | (c[1] << 8) | c[0];
  for (let i = 0; i < 4; i++) {
    pal[i] = rgba(BASE[i]);
    pal[i + 4] = rgba(BASE[i].map((v, k) => Math.round(v * 0.28 + bg[k] * 0.72)));
  }
  return pal;
});
const inkCss = (paper, ink) => ink === 1 ? 'rgb(226,44,32)' : ink === 2 ? 'rgb(40,80,220)' : paper ? '#fff' : '#181614';

function paint(canvas, s, onion) {
  const ctx = canvas.getContext('2d');
  if (!canvas._img) canvas._img = ctx.createImageData(W, H);
  const out = new Uint32Array(canvas._img.data.buffer);
  const pal = palettes[slot(s)[PAPER_OFF] ? 1 : 0];
  const px = new Uint8Array(api.memory.buffer, api.api_render(s, onion), W * H);
  for (let i = 0; i < W * H; i++) out[i] = pal[px[i]];
  ctx.putImageData(canvas._img, 0, 0);
}

function thumbUrl(s) {
  const px = new Uint8Array(api.memory.buffer, api.api_thumb(s), 64 * 48);
  const c = document.createElement('canvas');
  c.width = 64; c.height = 48;
  const ctx = c.getContext('2d');
  const img = ctx.createImageData(64, 48);
  const out = new Uint32Array(img.data.buffer);
  const pal = palettes[0];
  for (let i = 0; i < px.length; i++) out[i] = pal[px[i]];
  ctx.putImageData(img, 0, 0);
  return c.toDataURL('image/png');
}

// ---------------------------------------------------------------- storage
const store = {
  get(k, d = '') { try { return localStorage.getItem('qf-' + k) ?? d; } catch { return d; } },
  set(k, v) { try { localStorage.setItem('qf-' + k, v); } catch { /* private mode */ } },
};
function installId() {
  let id = store.get('id');
  if (!/^[0-9a-f]{16}$/.test(id)) {
    id = [...crypto.getRandomValues(new Uint8Array(8))].map((b) => b.toString(16).padStart(2, '0')).join('');
    store.set('id', id);
  }
  return id;
}
function serverUrl() {
  let s = store.get('server');
  if (!s && /\/app\/?/.test(location.pathname) && location.protocol.startsWith('http')) s = location.origin;
  return s.replace(/\/+$/, '');
}

let dbp;
function db() {
  dbp ??= new Promise((ok, fail) => {
    const r = indexedDB.open('quickflip', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('notes', { keyPath: 'id' });
    r.onsuccess = () => ok(r.result);
    r.onerror = () => fail(r.error);
  });
  return dbp;
}
async function dbDo(mode, fn) {
  const d = await db();
  return new Promise((ok, fail) => {
    const tx = d.transaction('notes', mode);
    const req = fn(tx.objectStore('notes'));
    tx.oncomplete = () => ok(req && req.result);
    tx.onerror = () => fail(tx.error);
  });
}
const dbAll = () => dbDo('readonly', (s) => s.getAll());
const dbGet = (id) => dbDo('readonly', (s) => s.get(id));
const dbPut = (rec) => dbDo('readwrite', (s) => s.put(rec));
const dbDel = (id) => dbDo('readwrite', (s) => s.delete(id));

// ---------------------------------------------------------------- small helpers
let toastTimer;
function toast(msg, ms = 2600) {
  const t = $('toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, ms);
}
function show(id) {
  for (const s of document.querySelectorAll('.screen')) s.hidden = s.id !== id;
  window.scrollTo(0, 0);
}
function el(tag, props = {}, ...kids) {
  const e = document.createElement(tag);
  Object.assign(e, props);
  for (const k of kids) e.append(k);
  return e;
}
function svgIcon(name) {
  const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  const u = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  u.setAttribute('href', '#i-' + name);
  s.append(u);
  return s;
}
const ago = (t) => {
  const s = Math.max(0, (Date.now() - t) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return Math.floor(s / 60) + ' min ago';
  if (s < 86400) return Math.floor(s / 3600) + ' h ago';
  if (s < 86400 * 30) return Math.floor(s / 86400) + ' d ago';
  return new Date(t).toLocaleDateString();
};
function trimUtf8(s, max) {
  const enc = new TextEncoder();
  while (enc.encode(s).length > max) s = [...s].slice(0, -1).join('');
  return s;
}
function download(bytes, name) {
  const a = el('a', { href: URL.createObjectURL(new Blob([bytes])), download: name });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}
function serverProblem(err) {
  const s = serverUrl();
  if (location.protocol === 'https:' && s.startsWith('http:'))
    return 'This page is on https, so the browser will not talk to an http server. Open the app from the server instead: ' + s + '/app/';
  return 'Could not reach the server at ' + s + (err ? ' (' + err.message + ')' : '') + '.';
}

// ---------------------------------------------------------------- library
let libTab = 'mine', libOffset = 0;

async function showLibrary() {
  stopPlayback();
  show('lib');
  history.replaceState(null, '', location.pathname);
  await fillLibrary(true);
}

async function fillLibrary(reset) {
  const grid = $('grid'), msg = $('lib-msg'), more = $('lib-more');
  for (const b of document.querySelectorAll('.tabs button')) b.classList.toggle('on', b.dataset.tab === libTab);
  if (reset) { grid.textContent = ''; libOffset = 0; }
  msg.hidden = true;
  more.hidden = true;
  if (libTab === 'mine') {
    const notes = (await dbAll()).sort((a, b) => b.updated - a.updated);
    const add = el('button', { className: 'card new' }, svgIcon('plus'), 'New note');
    add.onclick = () => openEditor(null);
    grid.append(add);
    for (const n of notes) {
      const card = el('button', { className: 'card' },
        el('img', { src: n.thumb, alt: '' }),
        el('div', { className: 'meta' }, el('b', { textContent: n.frames + (n.frames === 1 ? ' frame' : ' frames') }),
          el('span', { textContent: (n.uploadedId ? '↑ ' : '') + ago(n.updated) })));
      card.onclick = () => openEditor(n);
      grid.append(card);
    }
    return;
  }
  const s = serverUrl();
  if (!s) {
    msg.textContent = 'To see notes other people shared, put the server address in Settings (the gear, top right).';
    msg.hidden = false;
    return;
  }
  const sort = libTab === 'top-online' ? 'top' : 'new';
  try {
    const r = await fetch(`${s}/api/notes?sort=${sort}&offset=${libOffset}&limit=24`);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const { notes, total } = await r.json();
    if (!notes.length && !libOffset) {
      msg.textContent = 'Nothing on the server yet. Make a note and upload it from its menu.';
      msg.hidden = false;
    }
    for (const n of notes) {
      const card = el('button', { className: 'card' },
        el('img', { src: `${s}/api/notes/${n.id}.png`, alt: '', loading: 'lazy' }),
        el('div', { className: 'meta' }, el('b', { textContent: n.author || 'someone' }),
          el('span', { textContent: '★ ' + n.stars })));
      card.onclick = () => openOnline(n.id);
      grid.append(card);
    }
    libOffset += notes.length;
    more.hidden = libOffset >= total;
  } catch (e) {
    msg.textContent = serverProblem(e);
    msg.hidden = false;
  }
}

// ---------------------------------------------------------------- editor
const ed = {
  rec: null, index: 0, tool: 'pen', size: 1, pat: 0, layer: 0, onion: true,
  undo: [], redo: [], clip: null, dirty: false, noteDirty: false, drawing: null, playing: null,
};
const canvas = $('ed-canvas');

function count() { return api.api_count(); }

async function openEditor(rec) {
  stopPlayback();
  if (rec) {
    if (loadBytes(rec.data) !== 0) { toast('That note could not be opened (damaged file).'); return; }
  } else {
    api.api_new();
  }
  ed.rec = rec ? { ...rec } : { id: 'n' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6), created: Date.now() };
  ed.isNew = !rec;
  ed.noteDirty = false;
  ed.dirty = false;
  ed.index = -1;
  show('edit');
  history.pushState({ screen: 'edit' }, '', '#edit');
  loadFrame(0);
}

function commit() {
  if (!ed.dirty) return;
  if (api.api_set(ed.index, 0) !== 0) toast('Out of memory: this frame could not be kept.');
  ed.dirty = false;
  ed.noteDirty = true;
}

function loadFrame(i) {
  if (ed.index >= 0) commit();
  ed.index = Math.max(0, Math.min(count() - 1, i));
  api.api_get(ed.index, 0);
  if (ed.index > 0) api.api_get(ed.index - 1, 1);
  ed.undo = [];
  ed.redo = [];
  updateEditorUi();
  redraw();
}

let rafPending = false;
function redraw() {
  if (rafPending) return;
  rafPending = true;
  requestAnimationFrame(() => {
    rafPending = false;
    if (ed.playing) return;
    paint(canvas, 0, ed.onion && ed.index > 0 ? 1 : -1);
  });
}

function updateEditorUi() {
  const n = count(), f = slot(0), paper = f[PAPER_OFF];
  $('ed-pos').textContent = ed.index + 1;
  $('ed-count').textContent = n;
  $('ed-scrub').max = n;
  $('ed-scrub').value = ed.index + 1;
  $('ed-scrub').style.visibility = n < 2 ? 'hidden' : '';
  $('ed-prev').disabled = ed.index === 0;
  $('ed-next').disabled = ed.index >= n - 1;
  $('ed-add').disabled = $('ed-dup').disabled = n >= MAX_FRAMES;
  $('ed-undo').disabled = !ed.undo.length;
  $('ed-redo').disabled = !ed.redo.length;
  $('ed-paste').disabled = !ed.clip;
  $('ed-onion').classList.toggle('on', ed.onion);
  for (const b of document.querySelectorAll('[data-tool]')) b.classList.toggle('on', b.dataset.tool === ed.tool);
  for (const b of document.querySelectorAll('[data-layer]')) {
    const l = +b.dataset.layer;
    b.classList.toggle('on', l === ed.layer);
    b.querySelector('.sw').style.background = inkCss(paper, f[INK_OFF + l]);
  }
  const p = $('ed-paper').querySelector('.paper');
  p.style.background = paper ? '#181614' : '#fff';
  const d = $('ed-size').querySelector('.dot');
  const px = 3 + SIZES[ed.size] * 1.6;
  d.style.width = d.style.height = px + 'px';
  drawPatternIcon();
}

function drawPatternIcon() {
  const c = $('ed-pat').querySelector('canvas');
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, 16, 16);
  ctx.fillStyle = '#181614';
  for (let y = 0; y < 16; y++)
    for (let x = 0; x < 16; x++) {
      const on = ed.pat === 1 ? ((x + y) & 1) === 0 : ed.pat === 2 ? (y & 1) === 0 && ((x + (y & 2)) & 3) === 0 : ed.pat === 3 ? (y & 1) === 0 : true;
      if (on) ctx.fillRect(x, y, 1, 1);
    }
}

function pushUndo() {
  ed.undo.push(slot(0).slice());
  if (ed.undo.length > 40) ed.undo.shift();
  ed.redo = [];
}
function changed() {
  ed.dirty = true;
  updateEditorUi();
  redraw();
}

function framePoint(e) {
  const r = canvas.getBoundingClientRect();
  return [Math.floor((e.clientX - r.left) / r.width * W), Math.floor((e.clientY - r.top) / r.height * H)];
}

canvas.addEventListener('pointerdown', (e) => {
  if (ed.playing || ed.drawing || (e.pointerType === 'mouse' && e.button !== 0)) return;
  e.preventDefault();
  const [x, y] = framePoint(e);
  pushUndo();
  if (ed.tool === 'fill') {
    if (x >= 0 && y >= 0 && x < W && y < H) {
      if (api.api_fill(0, ed.layer, x, y, ed.pat, DRAW) !== 0) toast('Out of memory');
    }
    changed();
    return;
  }
  canvas.setPointerCapture(e.pointerId);
  ed.drawing = { id: e.pointerId, x, y };
  stroke(x, y);
});
function stroke(x, y) {
  const d = ed.drawing;
  const erase = ed.tool === 'eraser';
  api.api_line(0, ed.layer, d.x, d.y, x, y, SIZES[ed.size], erase ? 0 : ed.pat, erase ? ERASE : DRAW);
  d.x = x; d.y = y;
  ed.dirty = true;
  redraw();
}
canvas.addEventListener('pointermove', (e) => {
  if (!ed.drawing || e.pointerId !== ed.drawing.id) return;
  const evs = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
  for (const ev of evs.length ? evs : [e]) stroke(...framePoint(ev));
});
const endStroke = (e) => {
  if (!ed.drawing || e.pointerId !== ed.drawing.id) return;
  ed.drawing = null;
  updateEditorUi();
};
canvas.addEventListener('pointerup', endStroke);
canvas.addEventListener('pointercancel', endStroke);
canvas.addEventListener('contextmenu', (e) => e.preventDefault());

for (const b of document.querySelectorAll('[data-tool]'))
  b.onclick = () => { ed.tool = b.dataset.tool; updateEditorUi(); };
$('ed-size').onclick = () => { ed.size = (ed.size + 1) % SIZES.length; updateEditorUi(); };
$('ed-pat').onclick = () => { ed.pat = (ed.pat + 1) % 4; updateEditorUi(); };
for (const b of document.querySelectorAll('[data-layer]'))
  b.onclick = () => {
    const l = +b.dataset.layer;
    if (l !== ed.layer) { ed.layer = l; updateEditorUi(); return; }
    // tapping the chosen layer again changes its ink: auto -> red -> blue
    pushUndo();
    slot(0)[INK_OFF + l] = (slot(0)[INK_OFF + l] + 1) % 3;
    changed();
  };
$('ed-paper').onclick = () => { pushUndo(); slot(0)[PAPER_OFF] ^= 1; changed(); };
$('ed-onion').onclick = () => { ed.onion = !ed.onion; updateEditorUi(); redraw(); };
$('ed-undo').onclick = () => {
  if (!ed.undo.length) return;
  ed.redo.push(slot(0).slice());
  slot(0).set(ed.undo.pop());
  changed();
};
$('ed-redo').onclick = () => {
  if (!ed.redo.length) return;
  ed.undo.push(slot(0).slice());
  slot(0).set(ed.redo.pop());
  changed();
};
$('ed-copy').onclick = () => { ed.clip = slot(0).slice(); updateEditorUi(); toast('Frame copied'); };
$('ed-paste').onclick = () => { if (!ed.clip) return; pushUndo(); slot(0).set(ed.clip); changed(); };
$('ed-prev').onclick = () => loadFrame(ed.index - 1);
$('ed-next').onclick = () => loadFrame(ed.index + 1);
$('ed-scrub').oninput = (e) => loadFrame(+e.target.value - 1);

$('ed-add').onclick = () => {
  if (count() >= MAX_FRAMES) return;
  commit();
  // a blank frame with the same paper and inks
  api.api_clear(2);
  const s2 = slot(2), s0 = slot(0);
  s2[PAPER_OFF] = s0[PAPER_OFF]; s2[INK_OFF] = s0[INK_OFF]; s2[INK_OFF + 1] = s0[INK_OFF + 1];
  if (api.api_insert(ed.index + 1, 2) !== 0) { toast('Out of memory'); return; }
  ed.noteDirty = true;
  loadFrame(ed.index + 1);
};
$('ed-dup').onclick = () => {
  if (count() >= MAX_FRAMES) return;
  commit();
  if (api.api_insert(ed.index + 1, 0) !== 0) { toast('Out of memory'); return; }
  ed.noteDirty = true;
  loadFrame(ed.index + 1);
};
$('ed-del').onclick = () => {
  if (count() === 1) {
    if (api.api_blank(0)) return;
    pushUndo();
    const keep = [slot(0)[PAPER_OFF], slot(0)[INK_OFF], slot(0)[INK_OFF + 1]];
    api.api_clear(0);
    slot(0)[PAPER_OFF] = keep[0]; slot(0)[INK_OFF] = keep[1]; slot(0)[INK_OFF + 1] = keep[2];
    changed();
    return;
  }
  if (!api.api_blank(0) && !confirm('Delete frame ' + (ed.index + 1) + '?')) return;
  ed.dirty = false;
  api.api_delete(ed.index);
  ed.noteDirty = true;
  const i = Math.min(ed.index, count() - 1);
  ed.index = -1;
  loadFrame(i);
};

// ---- playing (in the editor and the player)
function startPlayback(canvasEl, from, onFrame, onEnd) {
  stopPlayback();
  const nv = noteView();
  const fps = SPEED_FPS[nv.getUint8(NOTE.speed)] || 6;
  const loop = nv.getUint8(NOTE.loop) & 1;
  const p = { i: from, timer: 0, t0: performance.now(), n0: from };
  const tick = () => {
    if (p.i >= count()) {
      if (!loop) { stopPlayback(); onEnd && onEnd(); return; }
      p.i = 0; p.n0 = 0; p.t0 = performance.now();
    }
    api.api_get(p.i, 2);
    paint(canvasEl, 2, -1);
    onFrame && onFrame(p.i);
    p.i++;
    // keep to the clock rather than adding up delays
    const due = p.t0 + (p.i - p.n0) * 1000 / fps;
    p.timer = setTimeout(tick, Math.max(0, due - performance.now()));
  };
  ed.playing = p;
  tick();
}
function stopPlayback() {
  if (!ed.playing) return;
  clearTimeout(ed.playing.timer);
  ed.playing = null;
}

$('ed-play').onclick = () => {
  if (ed.playing) { stopEditorPlay(); return; }
  commit();
  $('ed-play').querySelector('use').setAttribute('href', '#i-stop');
  startPlayback(canvas, 0, (i) => { $('ed-pos').textContent = i + 1; }, stopEditorPlay);
};
function stopEditorPlay() {
  stopPlayback();
  $('ed-play').querySelector('use').setAttribute('href', '#i-play');
  updateEditorUi();
  redraw();
}

// ---- saving and sharing
async function saveNote() {
  commit();
  if (ed.isNew && !ed.noteDirty && count() === 1 && api.api_blank(0)) return false;  // nothing drawn
  const nv = noteView();
  if (!nv.getUint32(NOTE.created, true)) nv.setUint32(NOTE.created, Math.floor(ed.rec.created / 1000), true);
  const id = installId();
  for (let i = 0; i < 8; i++) nv.setUint8(NOTE.authorId + i, parseInt(id.substr(i * 2, 2), 16));
  const name = new TextEncoder().encode(trimUtf8(store.get('author'), 32));
  const m = mem(), a = api.api_note() + NOTE.author;
  m.fill(0, a, a + 33);
  m.set(name, a);
  const bytes = saveBytes();
  if (!bytes) { toast('Saving failed (out of memory?)'); return false; }
  api.api_get(Math.min(nv.getUint16(NOTE.thumb, true), count() - 1), 2);
  ed.rec = { ...ed.rec, data: bytes, thumb: thumbUrl(2), frames: count(), updated: Date.now() };
  await dbPut(ed.rec);
  ed.isNew = false;
  ed.noteDirty = false;
  return true;
}

async function leaveEditor() {
  stopEditorPlay();
  try { await saveNote(); } catch (e) { toast('Could not save: ' + e.message); }
  showLibrary();
}
$('ed-back').onclick = () => history.back();

async function upload(bytes) {
  const s = serverUrl(), key = store.get('key');
  if (!s) throw new Error('put the server address in Settings first');
  if (!key) throw new Error('put your upload key in Settings first');
  let r;
  try {
    r = await fetch(s + '/api/notes', { method: 'POST', headers: { Authorization: 'Bearer ' + key }, body: bytes });
  } catch (e) { throw new Error(serverProblem(e)); }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
  return j.id;
}

function openMenu(title, build) {
  $('menu-title').textContent = title;
  const body = $('menu-body');
  body.textContent = '';
  build(body);
  $('dlg-menu').showModal();
}
function menuItem(body, text, fn, cls = '') {
  const b = el('button', { type: 'button', className: 'menu-item ' + cls, textContent: text });
  b.onclick = fn;
  body.append(b);
  return b;
}
function linkBox(body, url) {
  const box = el('div', { className: 'link-box', textContent: url });
  body.prepend(box);
  navigator.clipboard?.writeText(url).then(() => toast('Link copied'), () => {});
}

$('ed-menu').onclick = () => {
  stopEditorPlay();
  commit();
  const nv = noteView();
  openMenu('This note', (body) => {
    const sp = nv.getUint8(NOTE.speed);
    const out = el('output', { textContent: SPEED_FPS[sp] + ' fps' });
    const range = el('input', { type: 'range', min: 1, max: 8, value: sp });
    range.oninput = () => { nv.setUint8(NOTE.speed, +range.value); out.textContent = SPEED_FPS[+range.value] + ' fps'; ed.noteDirty = true; };
    body.append(el('label', { className: 'menu-row' }, 'Speed', range, out));
    const loop = el('input', { type: 'checkbox', checked: !!(nv.getUint8(NOTE.loop) & 1) });
    loop.onchange = () => { nv.setUint8(NOTE.loop, loop.checked ? 1 : 0); ed.noteDirty = true; };
    body.append(el('label', { className: 'menu-row' }, loop, 'Play over and over'));
    const th = nv.getUint16(NOTE.thumb, true);
    menuItem(body, th === ed.index ? 'This frame is the cover picture' : 'Use this frame as the cover picture (now frame ' + (th + 1) + ')', (e) => {
      nv.setUint16(NOTE.thumb, ed.index, true);
      ed.noteDirty = true;
      e.target.textContent = 'This frame is the cover picture';
    });
    menuItem(body, 'Save', async () => { if (await saveNote()) toast('Saved'); $('dlg-menu').close(); });
    menuItem(body, ed.rec.uploadedId ? 'Upload again (as a new copy)' : 'Upload to the server', async (e) => {
      e.target.disabled = true;
      e.target.textContent = 'Uploading…';
      try {
        await saveNote();
        const id = await upload(ed.rec.data);
        ed.rec.uploadedId = id;
        await dbPut(ed.rec);
        e.target.textContent = 'Uploaded';
        linkBox(body, serverUrl() + '/n/' + id);
      } catch (err) {
        e.target.disabled = false;
        e.target.textContent = 'Upload to the server';
        toast('Upload failed: ' + err.message, 5000);
      }
    });
    if (ed.rec.uploadedId) menuItem(body, 'Copy the link to the uploaded note', () => linkBox(body, serverUrl() + '/n/' + ed.rec.uploadedId));
    menuItem(body, 'Download the file (.qflip)', async () => {
      await saveNote();
      if (ed.rec.data) download(ed.rec.data, 'quickflip-' + ed.rec.id + '.qflip');
    });
    menuItem(body, 'Delete this note', async () => {
      if (!confirm('Delete this whole note from this device?')) return;
      await dbDel(ed.rec.id);
      ed.isNew = true; ed.noteDirty = false; ed.dirty = false;
      api.api_new();
      $('dlg-menu').close();
      history.back();
    }, 'danger');
  });
};

// ---------------------------------------------------------------- player
const pl = { id: null, bytes: null, author: '' };
const plCanvas = $('pl-canvas');

async function openOnline(id) {
  const s = serverUrl();
  try {
    const [info, file] = await Promise.all([
      fetch(`${s}/api/notes/${id}`).then((r) => { if (!r.ok) throw new Error(r.status === 404 ? 'that note is not on the server' : 'HTTP ' + r.status); return r.json(); }),
      fetch(`${s}/api/notes/${id}.qflip`).then((r) => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.arrayBuffer(); }),
    ]);
    openPlayer(new Uint8Array(file), info);
  } catch (e) {
    toast(e.message.startsWith('that') ? 'Sorry, ' + e.message : serverProblem(e), 5000);
  }
}

function openPlayer(bytes, info) {
  stopPlayback();
  if (loadBytes(bytes) !== 0) { toast('That note could not be opened (damaged file).'); return; }
  pl.id = info.id;
  pl.bytes = bytes;
  $('pl-author').textContent = info.author || 'someone';
  $('pl-info').textContent = `${count()} frames · ${ago(info.uploaded * 1000)}`;
  $('pl-star').hidden = false;
  $('pl-stars').textContent = info.stars;
  $('pl-scrub').max = count();
  show('play');
  history.pushState({ screen: 'play' }, '', '#n=' + info.id);
  playerPlay(0);
}
function playerPlay(from) {
  $('pl-play').querySelector('use').setAttribute('href', '#i-stop');
  startPlayback(plCanvas, from, (i) => { $('pl-scrub').value = i + 1; }, playerPaused);
}
function playerPaused() {
  stopPlayback();
  $('pl-play').querySelector('use').setAttribute('href', '#i-play');
}
$('pl-play').onclick = () => {
  if (ed.playing) { playerPaused(); return; }
  const at = +$('pl-scrub').value;
  playerPlay(at >= count() ? 0 : at);
};
$('pl-scrub').oninput = (e) => {
  playerPaused();
  api.api_get(+e.target.value - 1, 2);
  paint(plCanvas, 2, -1);
};
$('pl-back').onclick = () => history.back();
$('pl-star').onclick = async () => {
  const s = serverUrl(), key = store.get('key');
  try {
    const r = await fetch(`${s}/api/notes/${pl.id}/star`, { method: 'POST', headers: key ? { Authorization: 'Bearer ' + key } : {} });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    $('pl-stars').textContent = j.stars;
    $('pl-star').classList.add('on');
  } catch (e) { toast(serverProblem(e)); }
};
$('pl-menu').onclick = () => {
  playerPaused();
  openMenu('Shared note', (body) => {
    const s = serverUrl();
    menuItem(body, 'Copy the link', () => linkBox(body, `${s}/n/${pl.id}`));
    menuItem(body, 'Open as an animated GIF', () => window.open(`${s}/api/notes/${pl.id}.gif`, '_blank'));
    menuItem(body, 'Download the file (.qflip)', () => download(pl.bytes, 'quickflip-' + pl.id + '.qflip'));
    menuItem(body, 'Make my own copy to draw on', async () => {
      $('dlg-menu').close();
      const rec = { id: 'n' + Date.now().toString(36), created: Date.now(), data: pl.bytes, frames: count(), updated: Date.now() };
      api.api_get(0, 2);
      rec.thumb = thumbUrl(2);
      await dbPut(rec);
      history.replaceState(null, '', location.pathname);
      openEditor(rec);
    });
  });
};

// ---------------------------------------------------------------- settings
$('lib-settings').onclick = () => {
  $('set-author').value = store.get('author');
  $('set-server').value = store.get('server') || serverUrl();
  $('set-key').value = store.get('key');
  $('set-status').textContent = '';
  $('dlg-settings').showModal();
};
$('dlg-settings').addEventListener('close', async () => {
  if ($('dlg-settings').returnValue !== 'ok') return;
  store.set('author', trimUtf8($('set-author').value.trim(), 32));
  let s = $('set-server').value.trim().replace(/\/+$/, '');
  if (s && !/^https?:\/\//.test(s)) s = 'http://' + s;
  store.set('server', s);
  store.set('key', $('set-key').value.trim());
  if (!serverUrl()) { toast('Saved'); return; }
  try {
    const key = store.get('key');
    const r = await fetch(serverUrl() + '/api/info', { headers: key ? { Authorization: 'Bearer ' + key } : {} });
    const j = await r.json();
    toast(!key ? 'Server found. Add an upload key to share notes.' : j.key_valid ? 'Server found, upload key accepted.' : 'Server found, but it does not know that upload key.', 4000);
  } catch (e) { toast(serverProblem(e), 6000); }
  if (libTab !== 'mine') fillLibrary(true);
});

// ---------------------------------------------------------------- start
for (const b of document.querySelectorAll('.tabs button'))
  b.onclick = () => { libTab = b.dataset.tab; fillLibrary(true); };
$('lib-more').onclick = () => fillLibrary(false);

window.addEventListener('popstate', () => {
  if (!$('edit').hidden) leaveEditor();
  else if (!$('play').hidden) showLibrary();
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden && !$('edit').hidden) saveNote().catch(() => {});
});
setInterval(() => { if (!$('edit').hidden && (ed.dirty || ed.noteDirty) && !ed.drawing) saveNote().catch(() => {}); }, 30000);

(async () => {
  const startHash = location.hash;
  const r = await fetch('core/qflip.wasm');
  api = (await WebAssembly.instantiate(await r.arrayBuffer(), {})).instance.exports;
  window.qf = { api, ed, slot };  // for the tests
  await showLibrary();
  const m = /^#n=([a-z0-9]{8})$/.exec(startHash);
  if (m) openOnline(m[1]);
  if ('serviceWorker' in navigator && location.protocol !== 'file:') navigator.serviceWorker.register('sw.js').catch(() => {});
})();
