#!/usr/bin/env node
/**
 * Flycast WASM Demo Server
 *
 * Serves EmulatorJS + Flycast core with cross-origin isolation headers
 * required for SharedArrayBuffer (COEP/COOP).
 *
 * Usage:
 *   node server.js [port] [roms-dir]
 *
 * Examples:
 *   node server.js                          # port 3000, roms from ./roms/
 *   node server.js 3000 D:/Gaming/ROMs/DC   # port 3000, roms from D: drive
 *   ROMS_DIR=D:/path node server.js         # env var alternative
 *
 * Requires:
 *   - EmulatorJS data in ./data/ (copy from EmulatorJS release)
 *   - Flycast core in ./data/cores/flycast-wasm.data
 *   - BIOS files in ./bios/ (dc_boot.bin + dc_flash.bin)
 *   - ROM files in ./roms/ or specify path via arg/env (any .chd, .cdi, .gdi, .cue/.bin, .zip)
 */

const http = require('http')
const fs = require('fs')
const path = require('path')
const { randomUUID } = require('crypto')

const PORT = parseInt(process.argv[2] || '3000', 10)
const ROMS_DIR = process.argv[3] || process.env.ROMS_DIR || null
const ROOT = __dirname

// MIME types
const MIME = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
  '.7z': 'application/x-7z-compressed',
  '.bin': 'application/octet-stream',
  '.chd': 'application/octet-stream',
  '.cdi': 'application/octet-stream',
  '.gdi': 'application/octet-stream',
  '.cue': 'text/plain',
  '.iso': 'application/octet-stream',
  '.zip': 'application/zip',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.gif': 'image/gif',
  '.svg': 'image/svg+xml',
}

// Cross-origin isolation headers (required for SharedArrayBuffer)
const ISOLATION_HEADERS = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'cross-origin',
  'Access-Control-Allow-Origin': '*',
}

// Token-based file registry (prevents path traversal)
const fileTokens = new Map()

function registerFile(absolutePath) {
  const token = randomUUID()
  fileTokens.set(token, absolutePath)
  const basename = path.basename(absolutePath)
  return `/file/${token}/${encodeURIComponent(basename)}`
}

function getMime(filePath) {
  return MIME[path.extname(filePath).toLowerCase()] || 'application/octet-stream'
}

function serveStatic(res, filePath) {
  if (!fs.existsSync(filePath)) {
    res.writeHead(404, ISOLATION_HEADERS)
    res.end('Not found')
    return
  }

  const stat = fs.statSync(filePath)
  res.writeHead(200, {
    'Content-Type': getMime(filePath),
    'Content-Length': stat.size,
    'Cache-Control': 'no-store, no-cache, must-revalidate',
    'Pragma': 'no-cache',
    'Expires': '0',
    ...ISOLATION_HEADERS,
  })
  fs.createReadStream(filePath).pipe(res)
}

function serveTokenFile(res, req, token) {
  const absolutePath = fileTokens.get(token)
  if (!absolutePath || !fs.existsSync(absolutePath)) {
    res.writeHead(404, ISOLATION_HEADERS)
    res.end('Not found')
    return
  }

  const stat = fs.statSync(absolutePath)
  const range = req.headers.range

  // Support Range requests for large disk images
  if (range) {
    const parts = range.replace(/bytes=/, '').split('-')
    const start = parseInt(parts[0], 10)
    const end = parts[1] ? parseInt(parts[1], 10) : stat.size - 1
    const chunkSize = end - start + 1

    res.writeHead(206, {
      'Content-Range': `bytes ${start}-${end}/${stat.size}`,
      'Accept-Ranges': 'bytes',
      'Content-Length': chunkSize,
      'Content-Type': getMime(absolutePath),
      ...ISOLATION_HEADERS,
    })
    fs.createReadStream(absolutePath, { start, end }).pipe(res)
  } else {
    res.writeHead(200, {
      'Content-Type': getMime(absolutePath),
      'Content-Length': stat.size,
      'Accept-Ranges': 'bytes',
      ...ISOLATION_HEADERS,
    })
    fs.createReadStream(absolutePath).pipe(res)
  }
}

// Scan directories for files
function scanDir(dirPath, extensions) {
  if (!fs.existsSync(dirPath)) return []
  return fs.readdirSync(dirPath)
    .filter((f) => extensions.includes(path.extname(f).toLowerCase()))
    .map((f) => ({ name: f, path: path.join(dirPath, f) }))
}

// Build the launcher page (game picker)
function buildLauncherPage() {
  const biosDir = path.join(ROOT, 'bios')
  const romsDir = ROMS_DIR || path.join(ROOT, 'roms')

  const biosFiles = scanDir(biosDir, ['.bin'])
  const romFiles = scanDir(romsDir, ['.chd', '.cdi', '.gdi', '.cue', '.zip', '.iso', '.bin'])

  const hasBoot = biosFiles.some((f) => f.name.toLowerCase() === 'dc_boot.bin')
  const hasFlash = biosFiles.some((f) => f.name.toLowerCase() === 'dc_flash.bin')
  const biosOk = hasBoot && hasFlash

  // Filter out .bin files that are likely CD track data (not ROMs)
  const gameFiles = romFiles.filter((f) => {
    const ext = path.extname(f.name).toLowerCase()
    // .bin files are ambiguous — only include if no .cue file references them
    if (ext === '.bin') {
      const cueName = f.name.replace(/\.bin$/i, '.cue')
      const hasCue = romFiles.some((r) => r.name.toLowerCase() === cueName.toLowerCase())
      return !hasCue // exclude .bin if a matching .cue exists (it's a track, not a standalone ROM)
    }
    return true
  })

  const romListHtml = gameFiles.length > 0
    ? gameFiles.map((r) => {
        const url = registerFile(r.path)
        return `<div class="game-card" onclick="launchGame('${url}', '${encodeURIComponent(r.name)}')">`
          + `<div class="game-icon">&#9654;</div>`
          + `<div class="game-name">${r.name}</div>`
          + `</div>`
      }).join('\n')
    : '<p class="empty">No ROM files found. Place .chd, .cdi, .gdi, .cue, or .zip files in <code>demo/roms/</code></p>'

  // Register BIOS URLs
  let biosUrl = ''
  let extraBiosJson = '[]'
  if (biosOk) {
    const bootFile = biosFiles.find((f) => f.name.toLowerCase() === 'dc_boot.bin')
    const flashFile = biosFiles.find((f) => f.name.toLowerCase() === 'dc_flash.bin')
    biosUrl = registerFile(bootFile.path)
    extraBiosJson = JSON.stringify([{ filename: 'dc_flash.bin', url: registerFile(flashFile.path) }])
  }

  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Flycast WASM — Dreamcast Emulator</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      background: #0a0a1a;
      color: #e8e0f0;
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
    }
    .header {
      text-align: center;
      padding: 40px 20px 20px;
    }
    .header h1 {
      font-size: 28px;
      font-weight: 300;
      letter-spacing: 2px;
      color: #ff2a6d;
      margin-bottom: 8px;
    }
    .header p {
      color: #6a6480;
      font-size: 14px;
    }
    .status {
      display: flex;
      gap: 20px;
      justify-content: center;
      padding: 20px;
      flex-wrap: wrap;
    }
    .status-item {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 13px;
      color: #b0a8c0;
    }
    .dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
    }
    .dot.ok { background: #01ffc3; }
    .dot.missing { background: #ff003c; }
    .games {
      max-width: 800px;
      width: 100%;
      padding: 20px;
    }
    .games h2 {
      font-size: 16px;
      font-weight: 600;
      letter-spacing: 1px;
      text-transform: uppercase;
      color: #05d9e8;
      margin-bottom: 16px;
      padding-bottom: 8px;
      border-bottom: 1px solid rgba(5, 217, 232, 0.2);
    }
    .game-card {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 14px 16px;
      margin-bottom: 8px;
      background: rgba(255,255,255,0.03);
      border: 1px solid rgba(255,255,255,0.07);
      border-radius: 10px;
      cursor: pointer;
      transition: all 0.15s;
    }
    .game-card:hover {
      background: rgba(255, 42, 109, 0.08);
      border-color: rgba(255, 42, 109, 0.3);
      transform: translateX(4px);
    }
    .game-icon {
      color: #ff2a6d;
      font-size: 18px;
      width: 32px;
      text-align: center;
    }
    .game-name {
      font-size: 15px;
      font-weight: 500;
    }
    .empty {
      color: #6a6480;
      font-size: 14px;
      padding: 20px;
      text-align: center;
    }
    .empty code {
      background: rgba(255,255,255,0.06);
      padding: 2px 6px;
      border-radius: 4px;
      font-size: 13px;
    }
    .footer {
      padding: 30px;
      text-align: center;
      color: #3a3650;
      font-size: 12px;
    }
    .footer a { color: #ff2a6d; text-decoration: none; }
    .footer a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <div class="header">
    <h1>FLYCAST WASM</h1>
    <p>Sega Dreamcast emulation in the browser via WebAssembly</p>
  </div>
  <div class="status">
    <div class="status-item">
      <div class="dot ${hasBoot ? 'ok' : 'missing'}"></div>
      dc_boot.bin ${hasBoot ? '' : '(missing)'}
    </div>
    <div class="status-item">
      <div class="dot ${hasFlash ? 'ok' : 'missing'}"></div>
      dc_flash.bin ${hasFlash ? '' : '(missing)'}
    </div>
    <div class="status-item">
      <div class="dot ${gameFiles.length > 0 ? 'ok' : 'missing'}"></div>
      ${gameFiles.length} ROM${gameFiles.length !== 1 ? 's' : ''} found
    </div>
  </div>
  ${!biosOk ? '<p class="empty">Place <code>dc_boot.bin</code> and <code>dc_flash.bin</code> in <code>demo/bios/</code></p>' : ''}
  <div class="games">
    <h2>Games</h2>
    ${romListHtml}
  </div>
  <div class="footer">
    <a href="https://github.com/nasomers/flycast-wasm">github.com/nasomers/flycast-wasm</a>
  </div>
  <script>
    var biosUrl = '${biosUrl}';
    var extraBios = ${extraBiosJson};

    function launchGame(romUrl, romName) {
      if (!biosUrl) {
        alert('BIOS files missing. Place dc_boot.bin and dc_flash.bin in demo/bios/');
        return;
      }
      var params = new URLSearchParams({
        rom: romUrl,
        core: 'flycast',
        bios: biosUrl,
        extraBios: JSON.stringify(extraBios),
      });
      window.location.href = '/emulator?' + params.toString();
    }
  </script>
</body>
</html>`
}

// Build the emulator page (with all runtime patches)
function buildEmulatorPage(romUrl, biosUrl, extraBiosJson, coreOptionsJson) {
  const dataUrl = '/data/'

  let extraBiosFiles = []
  try { extraBiosFiles = JSON.parse(extraBiosJson || '[]') } catch {}

  let coreOptions = {}
  try { coreOptions = JSON.parse(coreOptionsJson || '{}') } catch {}

  // Default Dreamcast core options optimized for WASM
  const defaultOptions = {
    'reicast_boot_to_bios': 'disabled',
    'reicast_hle_bios': 'disabled',
    'reicast_threaded_rendering': 'disabled',
    'reicast_synchronous_rendering': 'disabled',
    'reicast_internal_resolution': '640x480',
    'reicast_mipmapping': 'disabled',
    'reicast_anisotropic_filtering': '1',
    'reicast_texupscale': 'disabled',
    'reicast_enable_rttb': 'disabled',
    'reicast_enable_purupuru': 'disabled',
    'reicast_alpha_sorting': 'per-strip (fast, least accurate)',
    'reicast_delay_frame_swapping': 'disabled',
    'reicast_frame_skipping': 'enabled',
    'reicast_framerate': 'normal',
    ...coreOptions,
  }

  const coreOptionsStr = Object.entries(defaultOptions)
    .map(([k, v]) => `${k} = "${v}"`)
    .join('\n') + '\n'

  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Flycast WASM</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { background: #000; overflow: hidden; width: 100vw; height: 100vh; }
    #game { width: 100vw; height: 100vh; }
  </style>
</head>
<body>
  <div id="game"></div>

  <script>
    // === Console noise suppression ===
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

    // === WebGL2 compatibility patches ===
    (function() {
      var origGetContext = HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext = function(type, attrs) {
        var ctx = origGetContext.call(this, type, attrs);
        if (ctx && (type === 'webgl2' || type === 'experimental-webgl2') && !ctx.__flycastPatched) {
          ctx.__flycastPatched = true;
          var origGetParam = ctx.getParameter.bind(ctx);

          // Patch A: GL_VERSION string override
          ctx.getParameter = function(pname) {
            if (pname === 0x1F02 || pname === ctx.VERSION) {
              return 'OpenGL ES 3.0 WebGL 2.0';
            }
            if (pname === 0x8B8C || pname === ctx.SHADING_LANGUAGE_VERSION) {
              return 'OpenGL ES GLSL ES 3.00';
            }
            return origGetParam(pname);
          };

          // Patch B: GL_INVALID_ENUM suppression
          var origGetError = ctx.getError.bind(ctx);
          ctx.getError = function() {
            var err = origGetError();
            while (err === 0x500) { err = origGetError(); }
            return err;
          };

          // Patch C: texParameteri/f unbound texture guard
          var texBindings = {};
          texBindings[ctx.TEXTURE_2D] = ctx.TEXTURE_BINDING_2D;
          texBindings[ctx.TEXTURE_CUBE_MAP] = ctx.TEXTURE_BINDING_CUBE_MAP;
          if (ctx.TEXTURE_3D) texBindings[ctx.TEXTURE_3D] = ctx.TEXTURE_BINDING_3D;
          if (ctx.TEXTURE_2D_ARRAY) texBindings[ctx.TEXTURE_2D_ARRAY] = ctx.TEXTURE_BINDING_2D_ARRAY;

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
        }
        return ctx;
      };
    })();

    // === EmulatorJS configuration ===
    EJS_DEBUG_XX = false;
    EJS_player = '#game';
    EJS_core = 'flycast';
    EJS_gameUrl = '${romUrl}';
    EJS_pathtodata = '${dataUrl}';
    ${biosUrl ? `EJS_biosUrl = '${biosUrl}';` : ''}
    EJS_startOnLoaded = true;
    EJS_color = '#ff2a6d';
    EJS_disableLocalStorage = true;
    EJS_defaultOptions = ${JSON.stringify(defaultOptions)};
  </script>

  <script>
    // === Unified startGame patch: BIOS + core options + system_directory ===
    // Single wrapper avoids race conditions between separate patches.
    (function() {
      var EXTRA_BIOS = ${JSON.stringify(extraBiosFiles)};
      var CORE_OPTS = ${JSON.stringify(coreOptionsStr)};
      var BIOS_FILES = ['dc_boot.bin', 'dc_flash.bin'];
      var iv = setInterval(function() {
        var emu = window.EJS_emulator;
        if (!emu || emu.__flycastPatched) return;
        emu.__flycastPatched = true;
        clearInterval(iv);
        var origStartGame = emu.startGame;
        emu.startGame = async function() {
          try {
            if (this.gameManager && this.gameManager.FS) {
              var FS = this.gameManager.FS;

              // 1. Fetch extra BIOS files (dc_flash.bin) BEFORE anything else
              for (var i = 0; i < EXTRA_BIOS.length; i++) {
                try {
                  var resp = await fetch(EXTRA_BIOS[i].url);
                  var buf = await resp.arrayBuffer();
                  FS.writeFile('/' + EXTRA_BIOS[i].filename, new Uint8Array(buf));
                } catch(e) {
                  console.error('[flycast-wasm] Failed to fetch extra BIOS:', EXTRA_BIOS[i].filename, e);
                }
              }

              // 2. Create /dc/ and copy ALL BIOS files there (now that dc_flash.bin exists)
              var biosDir = '/dc';
              try {
                if (!FS.analyzePath(biosDir).exists) FS.mkdir(biosDir);
              } catch(e) {}
              for (var j = 0; j < BIOS_FILES.length; j++) {
                var src = '/' + BIOS_FILES[j];
                var dst = biosDir + '/' + BIOS_FILES[j];
                try {
                  if (FS.analyzePath(src).exists && !FS.analyzePath(dst).exists) {
                    var data = FS.readFile(src);
                    FS.writeFile(dst, data);
                  }
                } catch(e) {}
              }

              // 3. Write core options before callMain
              if (this.Module && this.Module.callbacks) {
                var origCb = this.Module.callbacks.setupCoreSettingFile;
                this.Module.callbacks.setupCoreSettingFile = function(filePath) {
                  try { FS.writeFile(filePath, CORE_OPTS); } catch(e) {}
                  if (origCb) origCb(filePath);
                };
              }

              // 4. Set system_directory in retroarch.cfg
              // Flycast looks for BIOS in system_directory + '/dc/' — so set to '/'
              // so it finds them at /dc/dc_boot.bin and /dc/dc_flash.bin
              var cfgPath = '/home/web_user/.config/retroarch/retroarch.cfg';
              try {
                var cfg = new TextDecoder().decode(FS.readFile(cfgPath));
                if (cfg.indexOf('system_directory') === -1) {
                  FS.writeFile(cfgPath, cfg + 'system_directory = "/"\\n');
                }
              } catch(e) {}
            }
              // 5. Debug: verify BIOS placement
              try {
                var rootFiles = FS.readdir('/').filter(function(f) { return f !== '.' && f !== '..'; });
                console.log('[flycast-wasm] / contains:', rootFiles.join(', '));
                if (FS.analyzePath('/dc').exists) {
                  var dcFiles = FS.readdir('/dc').filter(function(f) { return f !== '.' && f !== '..'; });
                  console.log('[flycast-wasm] /dc/ contains:', dcFiles.join(', '));
                } else {
                  console.warn('[flycast-wasm] /dc/ does NOT exist!');
                }
              } catch(e) {}

          } catch(e) {
            console.error('[flycast-wasm] startGame patch failed:', e);
          }
          return origStartGame.apply(this, arguments);
        };
      }, 50);
    })();
  </script>

  <script src="${dataUrl}loader.js"></script>
</body>
</html>`
}

// HTTP server
const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://127.0.0.1:${PORT}`)
  const pathname = url.pathname

  // Launcher page
  if (pathname === '/' || pathname === '/index.html') {
    const html = buildLauncherPage()
    res.writeHead(200, {
      'Content-Type': 'text/html',
      'Content-Length': Buffer.byteLength(html),
      ...ISOLATION_HEADERS,
    })
    res.end(html)
    return
  }

  // Emulator page
  if (pathname === '/emulator') {
    const romUrl = url.searchParams.get('rom') || ''
    const biosUrlParam = url.searchParams.get('bios') || ''
    const extraBios = url.searchParams.get('extraBios') || '[]'
    const coreOptions = url.searchParams.get('coreOptions') || '{}'

    const html = buildEmulatorPage(romUrl, biosUrlParam, extraBios, coreOptions)
    res.writeHead(200, {
      'Content-Type': 'text/html',
      'Content-Length': Buffer.byteLength(html),
      ...ISOLATION_HEADERS,
    })
    res.end(html)
    return
  }

  // Token-based file serving (ROMs, BIOS)
  if (pathname.startsWith('/file/')) {
    const parts = pathname.split('/')
    const token = parts[2]
    serveTokenFile(res, req, token)
    return
  }

  // Static file serving (EmulatorJS data)
  if (pathname.startsWith('/data/')) {
    const relPath = pathname.slice(6) // strip /data/
    const filePath = path.join(ROOT, 'data', relPath)
    // Prevent path traversal
    if (!filePath.startsWith(path.join(ROOT, 'data'))) {
      res.writeHead(403, ISOLATION_HEADERS)
      res.end('Forbidden')
      return
    }
    serveStatic(res, filePath)
    return
  }

  // Screenshots
  if (pathname.startsWith('/screenshots/')) {
    const filePath = path.join(ROOT, '..', 'screenshots', pathname.slice(13))
    if (!filePath.startsWith(path.join(ROOT, '..', 'screenshots'))) {
      res.writeHead(403, ISOLATION_HEADERS)
      res.end('Forbidden')
      return
    }
    serveStatic(res, filePath)
    return
  }

  res.writeHead(404, ISOLATION_HEADERS)
  res.end('Not found')
})

server.listen(PORT, '127.0.0.1', () => {
  console.log('')
  console.log('  ╔══════════════════════════════════════════╗')
  console.log('  ║        FLYCAST WASM DEMO SERVER          ║')
  console.log('  ╠══════════════════════════════════════════╣')
  console.log(`  ║  http://127.0.0.1:${PORT}                   ║`)
  console.log('  ╚══════════════════════════════════════════╝')
  console.log('')

  // Check setup
  const dataDir = path.join(ROOT, 'data')
  const biosDir = path.join(ROOT, 'bios')
  const romsDir = ROMS_DIR || path.join(ROOT, 'roms')
  const coreFile = path.join(dataDir, 'cores', 'flycast-wasm.data')

  if (ROMS_DIR) {
    console.log(`  ROMs: ${ROMS_DIR}`)
    console.log('')
  }

  const checks = [
    { name: 'EmulatorJS data', ok: fs.existsSync(path.join(dataDir, 'loader.js')) },
    { name: 'Flycast core', ok: fs.existsSync(coreFile) },
    { name: 'dc_boot.bin', ok: fs.existsSync(path.join(biosDir, 'dc_boot.bin')) },
    { name: 'dc_flash.bin', ok: fs.existsSync(path.join(biosDir, 'dc_flash.bin')) },
    { name: 'ROMs directory', ok: fs.existsSync(romsDir) },
  ]

  checks.forEach((c) => {
    console.log(`  ${c.ok ? '\x1b[32m✓\x1b[0m' : '\x1b[31m✗\x1b[0m'} ${c.name}`)
  })
  console.log('')

  const missing = checks.filter((c) => !c.ok)
  if (missing.length > 0) {
    console.log('  Setup needed:')
    if (!fs.existsSync(path.join(dataDir, 'loader.js'))) {
      console.log('    Copy EmulatorJS data/ directory to demo/data/')
    }
    if (!fs.existsSync(coreFile)) {
      console.log('    Copy flycast-wasm.data to demo/data/cores/')
    }
    if (!fs.existsSync(path.join(biosDir, 'dc_boot.bin')) || !fs.existsSync(path.join(biosDir, 'dc_flash.bin'))) {
      console.log('    Place dc_boot.bin + dc_flash.bin in demo/bios/')
    }
    if (!fs.existsSync(romsDir)) {
      console.log('    Create demo/roms/ and add .chd/.cdi/.gdi/.cue/.zip files')
    }
    console.log('')
  }
})
