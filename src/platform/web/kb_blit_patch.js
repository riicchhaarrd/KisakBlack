// kb_blit_patch.js  (linked via --post-js)
//
// PERF: Emscripten's GL.blitOffscreenFramebuffer (the offscreen->canvas present blit done every
// emscripten_webgl_commit_frame) takes a SLOW manual-blit path whenever blitFramebuffer is
// "forbidden" (set when antialias is on), saving+restoring ~40 pieces of GL state via
// getParameter/getVertexAttrib each frame. On the PROXIED context each is a synchronous round-trip
// to the DOM thread — a Chrome CPU trace showed getParameter eating 21% of the DOM thread (and the
// render worker 14% blocked in SwapBuffers waiting on it).
//
// Chrome WebGL2 resolves MSAA via blitFramebuffer perfectly (the "forbid" was only ever needed for
// old Firefox < 67). So we just clear context.defaultFboForbidBlitFramebuffer and let Emscripten's
// own CHEAP fast path run: one blitFramebuffer + only a scissor/FBO save+restore (2 getParameter).
// That keeps the loading-screen present correct (the full no-restore version flickered black there)
// while removing the ~38 expensive per-frame getParameter round-trips. Reversible: delete the
// --post-js line in link_web_mt.sh.
(function () {
  try {
    if (typeof GL === 'undefined' || !GL || typeof GL.blitOffscreenFramebuffer !== 'function') return;
    var orig = GL.blitOffscreenFramebuffer;
    GL.blitOffscreenFramebuffer = function (context) {
      context.defaultFboForbidBlitFramebuffer = false;   // use the cheap blitFramebuffer fast path
      return orig(context);
    };
    if (typeof console !== 'undefined') console.log('[kb] blit: forced fast blitFramebuffer path (dropped slow getParameter save/restore)');
  } catch (e) {}
})();
