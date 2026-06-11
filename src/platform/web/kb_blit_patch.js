// kb_blit_patch.js  (linked via --post-js)
//
// PERF: Emscripten's GL.blitOffscreenFramebuffer (the offscreen->canvas present blit run every
// emscripten_webgl_commit_frame) queries GL state via getParameter every frame. On the PROXIED
// context EVERY getParameter is ~8ms — it forces the entire deep command queue + GPU to drain and
// round-trips to the DOM thread. A Chrome trace showed getParameter = 22% of the DOM thread even
// with just the 2-query "fast path"; only a ZERO-query blit removes it (measured +5-10fps).
//
// CoD re-sets its ENTIRE GL state every frame, so the blit needs to preserve NOTHING. Replace it
// with a minimal zero-getParameter version: disable scissor, blitFramebuffer (Chrome WebGL2
// resolves MSAA fine), bind FBO 0. The earlier loading-screen flicker was a present-outruns-render
// desync that happened with r_gpuSync OFF; r_gpuSync is default-ON now (paces present to render),
// which keeps the loading present coherent. Reversible: delete the --post-js line in link_web_mt.sh.
(function () {
  try {
    if (typeof GL === 'undefined' || !GL || typeof GL.blitOffscreenFramebuffer !== 'function') return;
    GL.blitOffscreenFramebuffer = function (context) {
      var gl = context.GLctx;
      gl.disable(0xC11 /*SCISSOR_TEST*/);   // engine re-enables per frame; blit must not be clipped
      if (gl.blitFramebuffer) {             // WebGL2: one resolve-blit, zero getParameter
        gl.bindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, context.defaultFbo);
        gl.bindFramebuffer(0x8CA9 /*DRAW_FRAMEBUFFER*/, null);
        gl.blitFramebuffer(0, 0, gl.canvas.width, gl.canvas.height,
                           0, 0, gl.canvas.width, gl.canvas.height,
                           0x4000 /*COLOR_BUFFER_BIT*/, 0x2600 /*NEAREST*/);
      } else {                              // WebGL1 fallback (manual blit, still zero getParameter)
        gl.bindFramebuffer(0x8D40 /*FRAMEBUFFER*/, null);
        gl.useProgram(context.blitProgram);
        gl.bindBuffer(0x8892 /*ARRAY_BUFFER*/, context.blitVB);
        gl.activeTexture(0x84C0 /*TEXTURE0*/);
        gl.bindTexture(0xDE1 /*TEXTURE_2D*/, context.defaultColorTarget);
        gl.disable(0xBE2); gl.disable(0xB44); gl.disable(0xB71); gl.disable(0xB90);
        if (context.defaultVao) { gl.bindVertexArray(context.defaultVao); }
        else {
          for (var i = 0; i < 16; ++i) (i === context.blitPosLoc ? gl.enableVertexAttribArray(i)
                                                                 : gl.disableVertexAttribArray(i));
          gl.vertexAttribPointer(context.blitPosLoc, 2, 0x1406 /*FLOAT*/, false, 0, 0);
        }
        gl.drawArrays(5 /*TRIANGLE_STRIP*/, 0, 4);
      }
      gl.bindFramebuffer(0x8D40 /*FRAMEBUFFER*/, null);   // engine binds its scene FBO next frame
    };
    if (typeof console !== 'undefined') console.log('[kb] blit: zero-getParameter present (gpusync paces it)');
  } catch (e) {}
})();
