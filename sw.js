// Offline cache for the app shell (only active on https or localhost).
// Other apps may live on the same site, so this only ever touches caches named qf-*.
const CACHE = 'qf-v1';
const SHELL = ['./', 'index.html', 'style.css', 'app.js', 'core/qflip.wasm', 'manifest.webmanifest', 'icon.svg', 'icon-192.png'];
self.addEventListener('install', (e) => e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting())));
self.addEventListener('activate', (e) => e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k.startsWith('qf-') && k !== CACHE).map((k) => caches.delete(k)))).then(() => self.clients.claim())));
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  // notes on the server are never cached here
  if (e.request.method !== 'GET' || url.origin !== location.origin || url.pathname.includes('/api/')) return;
  // network first, fall back to cache
  e.respondWith(fetch(e.request).then((r) => { const copy = r.clone(); caches.open(CACHE).then((c) => c.put(e.request, copy)); return r; }).catch(() => caches.match(e.request)));
});
