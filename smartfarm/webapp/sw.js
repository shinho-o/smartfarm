const CACHE_NAME = 'smartfarm-v1';
const ASSETS = [
    '/',
    '/index.html',
    '/manifest.json',
    'https://unpkg.com/mqtt/dist/mqtt.min.js'
];

self.addEventListener('install', e => {
    e.waitUntil(
        caches.open(CACHE_NAME).then(c => c.addAll(ASSETS))
    );
    self.skipWaiting();
});

self.addEventListener('fetch', e => {
    e.respondWith(
        fetch(e.request).catch(() => caches.match(e.request))
    );
});
