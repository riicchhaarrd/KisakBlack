// kb_blit_patch.js  (linked via --post-js)
//
// PERF: Emscripten's GL.blitOffscreenFramebuffer (the offscreen->canvas present blit, run every
// emscripten_webgl_commit_frame) queries GL state via getParameter every frame. On the PROXIED
// context EVERY getParameter is ~8ms (drains the whole command queue + GPU + DOM round-trip) — a
// Chrome trace showed getParameter = 22% of the DOM thread; only a ZERO-query blit removes it.
//
// Use Emscripten's MANUAL texture-blit (sample the offscreen color as a TEXTURE, draw a fullscreen
// quad to the canvas) — NOT gl.blitFramebuffer. blitFramebuffer(READ=defaultFbo, DRAW=null) hits a
// "Blit feedback loop: read and draw framebuffers are the same" error in states where defaultFbo
// resolves to the canvas (loading screen -> black flicker). The texture-blit has no framebuffer
// read/draw conflict. We keep that path but DROP all the getParameter save/restore: CoD re-sets its
// entire GL state every frame, so trashing program/buffer/texture/blend here is harmless.
// Reversible: delete the --post-js line in link_web_mt.sh.
(function () {
  try {
    if (typeof GL === 'undefined' || !GL || typeof GL.blitOffscreenFramebuffer !== 'function') return;
    GL.blitOffscreenFramebuffer = function (context) {
      var gl = context.GLctx;
      gl.bindFramebuffer(0x8D40 /*FRAMEBUFFER*/, null);     // draw to the canvas default framebuffer
      gl.disable(0xC11 /*SCISSOR_TEST*/);                   // engine re-enables per frame
      gl.disable(0xBE2 /*BLEND*/); gl.disable(0xB44 /*CULL_FACE*/);
      gl.disable(0xB71 /*DEPTH_TEST*/); gl.disable(0xB90 /*STENCIL_TEST*/);
      gl.useProgram(context.blitProgram);
      gl.activeTexture(0x84C0 /*TEXTURE0*/);
      gl.bindTexture(0xDE1 /*TEXTURE_2D*/, context.defaultColorTarget);  // sample offscreen color
      if (context.defaultVao) {                             // WebGL2: VAO already has blitVB+attrib
        gl.bindVertexArray(context.defaultVao);
      } else {
        gl.bindBuffer(0x8892 /*ARRAY_BUFFER*/, context.blitVB);
        for (var i = 0; i < 16; ++i) (i === context.blitPosLoc ? gl.enableVertexAttribArray(i)
                                                               : gl.disableVertexAttribArray(i));
        gl.vertexAttribPointer(context.blitPosLoc, 2, 0x1406 /*FLOAT*/, false, 0, 0);
      }
      gl.drawArrays(5 /*TRIANGLE_STRIP*/, 0, 4);
    };
    if (typeof console !== 'undefined') console.log('[kb] blit: zero-getParameter texture-blit (no feedback loop)');
  } catch (e) {}
})();
