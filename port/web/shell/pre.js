// pre.js (linked into halo.js): a crash in one of the game's threads is
// logged with its stack, which the page's console otherwise leaves out
if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  self.addEventListener('error', (event) => {
    console.error('the game crashed: ' + ((event.error && event.error.stack) || event.message));
  });
}
