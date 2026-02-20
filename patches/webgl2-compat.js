/**
 * WebGL2 Compatibility Patches for Flycast WASM
 *
 * These patches are injected into the emulator page's <script> block BEFORE
 * EmulatorJS loads. They intercept WebGL2 context creation and fix three
 * incompatibilities between Flycast/RetroArch and the WebGL2 API:
 *
 * 1. GL_VERSION string — Returns proper "OpenGL ES 3.0" instead of garbage
 * 2. GL_INVALID_ENUM suppression — Prevents RetroArch from aborting video init
 * 3. texParameter guard — Prevents console spam from unbound texture calls
 *
 * Additionally, console.warn is filtered to suppress harmless Emscripten noise.
 */

// --- Console noise suppression ---
// __syscall_mprotect: WASM has no mprotect, harmless no-op (~210/session)
// "is not a valid value": Flycast core option warnings for unrecognized values
(function() {
  var origWarn = console.warn;
  console.warn = function() {
    if (arguments.length > 0 && typeof arguments[0] === 'string') {
      var msg = arguments[0];
      if (msg.indexOf('__syscall_mprotect') !== -1) return;
      if (msg.indexOf('is not a valid value') !== -1) return;
    }
    return origWarn.apply(console, arguments);
  };
})();

// --- WebGL2 context patches ---
(function() {
  var origGetContext = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function(type, attrs) {
    var ctx = origGetContext.call(this, type, attrs);
    if (ctx && (type === 'webgl2' || type === 'experimental-webgl2') && !ctx.__flycastPatched) {
      ctx.__flycastPatched = true;

      // 1. GL_VERSION / GL_SHADING_LANGUAGE_VERSION override
      // Flycast's gles.cpp and RetroArch's gl2 driver both call glGetString(GL_VERSION)
      // to detect the GL profile. In WASM, this can return garbage before the context
      // is fully initialized, causing Flycast to select incompatible desktop GL3 shaders.
      var origGetParam = ctx.getParameter.bind(ctx);
      ctx.getParameter = function(pname) {
        if (pname === 0x1F02 || pname === ctx.VERSION) {
          return 'OpenGL ES 3.0 WebGL 2.0';
        }
        if (pname === 0x8B8C || pname === ctx.SHADING_LANGUAGE_VERSION) {
          return 'OpenGL ES GLSL ES 3.00';
        }
        return origGetParam(pname);
      };

      // 2. GL_INVALID_ENUM (0x500) suppression
      // WebGL2 generates GL_INVALID_ENUM for certain FBO operations that are valid
      // on desktop GL. RetroArch's gl2_check_error() calls getError() after FBO init
      // and aborts the video driver if any error is present. Suppressing 0x500 lets
      // the video driver init succeed — the actual rendering works fine.
      var origGetError = ctx.getError.bind(ctx);
      ctx.getError = function() {
        var err = origGetError();
        while (err === 0x500) { err = origGetError(); }
        return err;
      };

      // 3. texParameteri/f guard — prevent unbound texture calls
      // Flycast calls texParameteri/f before binding a texture (valid on desktop GL,
      // produces GL_INVALID_OPERATION on WebGL2). Without this, hundreds of synchronous
      // console.error() calls per frame cause massive lag on texture-heavy games.
      var texBindings = {};
      texBindings[ctx.TEXTURE_2D] = ctx.TEXTURE_BINDING_2D;
      texBindings[ctx.TEXTURE_CUBE_MAP] = ctx.TEXTURE_BINDING_CUBE_MAP;
      texBindings[ctx.TEXTURE_3D] = ctx.TEXTURE_BINDING_3D;
      texBindings[ctx.TEXTURE_2D_ARRAY] = ctx.TEXTURE_BINDING_2D_ARRAY;

      var origTexParameteri = ctx.texParameteri.bind(ctx);
      ctx.texParameteri = function(target, pname, param) {
        var b = texBindings[target];
        if (b && !origGetParam(b)) return;
        return origTexParameteri(target, pname, param);
      };
      var origTexParameterf = ctx.texParameterf.bind(ctx);
      ctx.texParameterf = function(target, pname, param) {
        var b = texBindings[target];
        if (b && !origGetParam(b)) return;
        return origTexParameterf(target, pname, param);
      };

      console.log('[flycast-wasm] Patched WebGL2 context');
    }
    return ctx;
  };
})();
