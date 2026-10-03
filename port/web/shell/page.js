// The page: checks the browser, starts the game (halo.js) with its canvas,
// shows the frames the game posts, tells the game the page's size, and joins
// the site's network (net.js).
'use strict';

(() => {
  const screen = document.getElementById('screen');
  const canvas = document.getElementById('canvas');
  const status = document.getElementById('status');
  const statusText = document.getElementById('status-text');
  const statusDetail = document.getElementById('status-detail');

  function showStatus(text, detail, error) {
    status.classList.remove('hidden');
    status.classList.toggle('error', !!error);
    statusText.textContent = text;
    statusDetail.textContent = detail || '';
  }

  // what the browser must have
  const missing = [];
  if (!window.crossOriginIsolated) missing.push('cross-origin isolation (the server must send COOP and COEP headers)');
  if (typeof SharedArrayBuffer === 'undefined') missing.push('SharedArrayBuffer');
  if (!canvas.transferControlToOffscreen) missing.push('OffscreenCanvas');
  if (!document.createElement('canvas').getContext('webgl2')) missing.push('WebGL 2');
  if (missing.length) {
    showStatus('This browser cannot run the game.', 'Missing: ' + missing.join(', '), true);
    return;
  }

  const view = screen.getContext('bitmaprenderer');
  let shown = false;

  // the page's size in the device's pixels: the game draws that shape at
  // that resolution
  function displaySize() {
    const ratio = window.devicePixelRatio || 1;
    const rect = screen.getBoundingClientRect();
    return [Math.max(1, Math.round(rect.width * ratio)), Math.max(1, Math.round(rect.height * ratio))];
  }

  function tellSize() {
    if (Module._web_set_display_size) Module._web_set_display_size(...displaySize());
  }

  // ?debug: the game's log on screen too, as the native builds show it
  const query = new URLSearchParams(location.search);

  window.Module = {
    canvas,
    preRun: [() => {
      if (query.has('debug')) Module.ENV.HALO_CONSOLE_LOG = 'all';
    }],
    print: (text) => console.log(text),
    printErr: (text) => console.log(text),
    // a frame from the game's thread (web_platform.c web_present_frame)
    haloFrame(bitmap, pending) {
      if (screen.width !== bitmap.width || screen.height !== bitmap.height) {
        screen.width = bitmap.width;
        screen.height = bitmap.height;
      }
      view.transferFromImageBitmap(bitmap);
      if (!shown) {
        shown = true;
        status.classList.add('hidden');
        canvas.focus();
      }
      // the game draws the next one once this one is on the screen
      requestAnimationFrame(() => {
        Atomics.sub(Module.HEAP32, pending >> 2, 1);
        Atomics.notify(Module.HEAP32, pending >> 2);
      });
    },
    // the network's rings exist (posix_bridge.c)
    haloNetAttach() {
      const pointer = Module._malloc(36);
      const base = Module._web_net_layout(pointer);
      const layout = Array.from(new Uint32Array(Module.HEAPU8.buffer, pointer, 9));
      Module._free(pointer);
      HaloNet.attach(Module.HEAPU8.buffer, base, layout, window.HALO_SITE);
    },
    onRuntimeInitialized() {
      tellSize();
      showStatus('Starting…');
    },
    onAbort(reason) {
      showStatus('The game stopped.', String(reason), true);
    },
  };

  new ResizeObserver(tellSize).observe(screen);
  canvas.addEventListener('pointerdown', () => canvas.focus());
  showStatus('Loading…');
  const script = document.createElement('script');
  script.src = 'halo.js';
  script.onerror = () => showStatus('The game did not load.', 'halo.js', true);
  document.body.appendChild(script);
})();
