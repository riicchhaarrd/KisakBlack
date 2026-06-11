// kb_blit_patch.js  (linked via --post-js)
//
// The offscreen->canvas present blit (GL.blitOffscreenFramebuffer, run every
// emscripten_webgl_commit_frame) queries GL state via getParameter every frame; on the PROXIED
// context each getParameter is ~8ms (drains the command queue + GPU + DOM round-trip) — 22% of the
// DOM thread. A zero-getParameter blit removes it (+5-10fps) BUT reimplementing the blit correctly
// is fiddly (blitFramebuffer hits a read/draw feedback loop; the manual texture-blit must replicate
// Emscripten's exact program/buffer/attrib setup or it draws garbage/upside-down).
//
// So: DEFAULT keeps Emscripten's original blit (correct + smooth, just slower). ?fastblit=1 opts
// into the zero-getParameter manual texture-blit (matches Emscripten's manual path operations,
// minus the getParameter save/restore — CoD re-sets all GL state every frame so the trash is fine).
// Reversible / experimental until it's proven clean. Reload with ?fastblit=1 to A/B fps vs feel.
(function () {
  try {
    var loc = (typeof location !== 'undefined') ? location : (typeof self !== 'undefined' && self.location);
    var on = loc && loc.search && loc.search.indexOf('fastblit=1') >= 0;
    if (!on) return;   // default: leave Emscripten's original (correct) blit in place
    if (typeof GL === 'undefined' || !GL || typeof GL.blitOffscreenFramebuffer !== 'function') return;
    GL.blitOffscreenFramebuffer = function (context) {
      var gl = context.GLctx;
      gl.disable(0xC11 /*SCISSOR_TEST*/);
      gl.bindFramebuffer(0x8D40 /*FRAMEBUFFER*/, null);          // draw to the canvas
      gl.useProgram(context.blitProgram);
      gl.activeTexture(0x84C0 /*TEXTURE0*/);
      gl.bindTexture(0xDE1 /*TEXTURE_2D*/, context.defaultColorTarget);   // sample offscreen color
      gl.disable(0xBE2 /*BLEND*/); gl.disable(0xB44 /*CULL_FACE*/);
      gl.disable(0xB71 /*DEPTH_TEST*/); gl.disable(0xB90 /*STENCIL_TEST*/);
      if (context.defaultVao) gl.bindVertexArray(context.defaultVao);
      else for (var i = 0; i < 16; ++i) (i === context.blitPosLoc ? gl.enableVertexAttribArray(i)
                                                                  : gl.disableVertexAttribArray(i));
      gl.bindBuffer(0x8892 /*ARRAY_BUFFER*/, context.blitVB);    // after VAO bind, before the pointer
      gl.vertexAttribPointer(context.blitPosLoc, 2, 0x1406 /*FLOAT*/, false, 0, 0);
      gl.drawArrays(5 /*TRIANGLE_STRIP*/, 0, 4);
    };
    if (typeof console !== 'undefined') console.log('[kb] blit: ?fastblit zero-getParameter texture-blit (experimental)');
  } catch (e) {}
})();
