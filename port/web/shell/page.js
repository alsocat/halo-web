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

  // (the frames are opaque: web_platform.c)
  const view = screen.getContext('bitmaprenderer', { alpha: false });
  let shown = false;
  let framesShown = 0;
  let brightness = null;
  const started = performance.now();

  // the game's last lines, for the report below
  const logLines = [];
  function remember(text) {
    console.log(text);
    logLines.push(String(text));
    if (logLines.length > 200) logLines.shift();
  }

  // A report to the site's server (its access log, port/web/nginx.conf), so a
  // player's problem can be seen there: the browser, its GPU, how far the game
  // got and its last lines. Once 30 seconds in, and once if the game stops.
  const reported = new Set();
  function report(reason) {
    if (reported.has(reason)) return;
    reported.add(reason);
    let gpu = 'no WebGL 2';
    try {
      const gl = document.createElement('canvas').getContext('webgl2');
      const info = gl && gl.getExtension('WEBGL_debug_renderer_info');
      if (gl) gpu = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    } catch (error) {
      gpu = 'WebGL error ' + error;
    }
    const text = [
      reason,
      navigator.userAgent,
      'gpu ' + gpu,
      'isolated ' + window.crossOriginIsolated + ', memory ' + (navigator.deviceMemory || '?') + ' GB, cores ' +
        (navigator.hardwareConcurrency || '?'),
      'frames ' + framesShown + ', brightness ' + brightness + ', after ' + Math.round((performance.now() - started) / 1000) + ' s',
      'status ' + (status.classList.contains('hidden') ? '(hidden)' : statusText.textContent + ' ' + statusDetail.textContent),
      ...logLines.slice(-30),
    ].join('\n');
    fetch('report?' + new URLSearchParams({ d: text.slice(0, 6000) }), { keepalive: true }).catch(() => {});
  }
  setTimeout(() => report('30 seconds'), 30000);
  addEventListener('error', (event) => {
    remember('page error: ' + event.message);
    report('page error');
  });

  // how bright one frame is (0 is black), once 20 seconds in
  function measure(bitmap) {
    try {
      const sample = document.createElement('canvas');
      sample.width = sample.height = 16;
      const context = sample.getContext('2d');
      context.drawImage(bitmap, 0, 0, 16, 16);
      const pixels = context.getImageData(0, 0, 16, 16).data;
      let total = 0;
      for (let index = 0; index < pixels.length; index += 4) total += pixels[index] + pixels[index + 1] + pixels[index + 2];
      brightness = Math.round(total / (16 * 16 * 3));
    } catch (error) {
      brightness = 'error ' + error;
    }
  }

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
      // and any setting by its environment name (port/linux/src/port_config.c),
      // ?HALO_NETWORK_TEST=join for one
      for (const [name, value] of query) {
        if (/^HALO_[A-Z0-9_]+$/.test(name)) Module.ENV[name] = value;
      }
    }],
    print: remember,
    printErr: remember,
    // a frame from the game's thread (web_platform.c web_present_frame)
    haloFrame(bitmap, pending) {
      if (screen.width !== bitmap.width || screen.height !== bitmap.height) {
        screen.width = bitmap.width;
        screen.height = bitmap.height;
      }
      framesShown++;
      if (brightness === null && performance.now() - started > 20000) measure(bitmap);
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
      attachGamepads();
      showStatus('Starting…');
    },
    onAbort(reason) {
      showStatus('The game stopped.', String(reason), true);
      remember('stopped: ' + reason);
      report('stopped');
    },
  };

  // controllers: browsers give them to this thread only, so the page copies
  // each one into the game's memory every frame (port/web/src/web_input.c),
  // and plays the game's rumble
  let gamepads = null;

  function attachGamepads() {
    const pointer = Module._malloc(32);
    const base = Module._web_gamepad_state(pointer);
    const layout = Array.from(new Int32Array(Module.HEAPU8.buffer, pointer, 8));
    Module._free(pointer);
    const buffer = Module.HEAPU8.buffer;
    const [size, connected, axes, buttons, rumbleLow, rumbleHigh, count, buttonCount] = layout;
    gamepads = { buttonCount, slots: [] };
    for (let index = 0; index < count; index++) {
      const at = base + index * size;
      gamepads.slots.push({
        ints: new Int32Array(buffer, at, size / 4),
        connected: connected / 4,
        rumbleLow: rumbleLow / 4,
        rumbleHigh: rumbleHigh / 4,
        axes: new Float32Array(buffer, at + axes, 4),
        buttons: new Float32Array(buffer, at + buttons, buttonCount),
        rumbling: false,
      });
    }
    requestAnimationFrame(pollGamepads);
  }

  function pollGamepads() {
    requestAnimationFrame(pollGamepads);
    const list = navigator.getGamepads ? navigator.getGamepads() : [];
    gamepads.slots.forEach((slot, index) => {
      const pad = list[index];
      const usable = !!(pad && pad.connected && pad.buttons.length >= gamepads.buttonCount);
      if (usable) {
        for (let axis = 0; axis < 4; axis++) slot.axes[axis] = pad.axes[axis] || 0;
        for (let button = 0; button < gamepads.buttonCount; button++) {
          slot.buttons[button] = pad.buttons[button].value || (pad.buttons[button].pressed ? 1 : 0);
        }
        const low = Atomics.load(slot.ints, slot.rumbleLow);
        const high = Atomics.load(slot.ints, slot.rumbleHigh);
        const actuator = pad.vibrationActuator;
        if (actuator && (low || high)) {
          actuator.playEffect('dual-rumble', { duration: 100, strongMagnitude: low / 65535, weakMagnitude: high / 65535 })
            .catch(() => {});
          slot.rumbling = true;
        } else if (actuator && slot.rumbling) {
          if (actuator.reset) actuator.reset().catch(() => {});
          slot.rumbling = false;
        }
      }
      Atomics.store(slot.ints, slot.connected, usable ? 1 : 0);
    });
  }

  new ResizeObserver(tellSize).observe(screen);
  canvas.addEventListener('pointerdown', () => canvas.focus());
  showStatus('Loading…');
  const script = document.createElement('script');
  script.src = 'halo.js';
  script.onerror = () => showStatus('The game did not load.', 'halo.js', true);
  document.body.appendChild(script);
})();
