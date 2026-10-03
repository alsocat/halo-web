// pre.js (linked into halo.js): a crash in one of the game's threads is
// logged with its stack, which the page's console otherwise leaves out
if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  self.addEventListener('error', (event) => {
    const text = 'the game crashed: ' + ((event.error && event.error.stack) || event.message);
    console.error(text);
    // (to the page's log too, for its report: page.js)
    if (typeof err === 'function') err(text);
  });
}

// ?HALO_WEB_PROFILE=1 (the game's thread sees it as an environment
// variable): the WebGL calls the game makes, counted by name, the most
// frequent logged every 5 seconds per frame, with the time each name took
// in this thread. (Chrome runs each call again in its GPU process.)
if (typeof WebGL2RenderingContext !== 'undefined' && typeof WorkerGlobalScope !== 'undefined' &&
    self instanceof WorkerGlobalScope) {
  self.haloProfileGL = () => {
    if (self.haloProfiling) return;
    self.haloProfiling = true;
    const counts = {}, times = {};
    const proto = WebGL2RenderingContext.prototype;
    for (const name of Object.getOwnPropertyNames(proto)) {
      const descriptor = Object.getOwnPropertyDescriptor(proto, name);
      if (!descriptor || typeof descriptor.value !== 'function' || name === 'constructor') continue;
      const call = descriptor.value;
      proto[name] = function (...args) {
        const begin = performance.now();
        const result = call.apply(this, args);
        times[name] = (times[name] || 0) + performance.now() - begin;
        counts[name] = (counts[name] || 0) + 1;
        return result;
      };
    }
    let frames = 0;
    self.haloProfileFrame = () => {
      if (++frames < 150) return;
      const total = Object.values(counts).reduce((a, b) => a + b, 0);
      const top = Object.keys(counts).sort((a, b) => counts[b] - counts[a]).slice(0, 18)
        .map((name) => `${name} ${(counts[name] / frames).toFixed(1)} (${(times[name] / frames).toFixed(2)} ms)`);
      err(`GL calls per frame: ${(total / frames).toFixed(0)}: ${top.join(', ')}`);
      for (const name in counts) { counts[name] = 0; times[name] = 0; }
      frames = 0;
    };
  };
}
