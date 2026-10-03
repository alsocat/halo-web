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
