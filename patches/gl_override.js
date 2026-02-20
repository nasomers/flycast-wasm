mergeInto(LibraryManager.library, {
  glGetString__deps: ['malloc'],
  glGetString: function(name) {
    if (typeof GL === 'undefined') GL = {};
    if (typeof GL.stringCache === 'undefined') GL.stringCache = {};
    if (typeof GL.stringCache[name] === 'number') return GL.stringCache[name];
    var str = null;
    if (name === 0x1F02) {
      str = 'OpenGL ES 3.0 WebGL 2.0';
    } else if (name === 0x8B8C) {
      str = 'OpenGL ES GLSL ES 3.00';
    } else {
      var ctx = (typeof GL.currentContext === 'object' && GL.currentContext) ? GL.currentContext.GLctx : null;
      str = ctx ? (ctx.getParameter(name) || '') : '';
    }
    var buf = _malloc(str.length + 1);
    stringToUTF8(str, buf, str.length + 1);
    GL.stringCache[name] = buf;
    return buf;
  }
});
