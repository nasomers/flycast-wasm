/**
 * Flycast WASM Test Harness
 *
 * Automated test for Flycast WASM core in EmulatorJS.
 * Launches demo server, opens browser, loads ROM, captures console output + screenshot.
 *
 * Usage: node flycast-wasm-test.js
 *
 * Prerequisites:
 *   npm install playwright pngjs
 *   ROMs in D:\Gaming\ROMs\Dreamcast\
 *   BIOS files in place for demo server
 *
 * Output:
 *   upstream/test-results.json   -structured result with pass/fail + diagnostics
 *   upstream/test-console.log    -full raw console output
 *   upstream/test-screenshot.png -screenshot after test duration
 *
 * Result statuses:
 *   PASS           -visual output detected, no crashes
 *   FAIL_CRASH     -runtime error or abort detected
 *   FAIL_BLACK     -no visual output (black screen)
 *   FAIL_NO_VISUAL -some pixels but no meaningful content (< threshold)
 *   FAIL_BEHAVIOR  -no crash but emulation behavior is broken (e.g. mainloop cycling)
 *   ERROR          -harness itself failed
 */

const { chromium } = require('playwright');
const { spawn, execSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

// === Config ===
const PROJECT_DIR = 'C:\\DEV Projects\\flycast-wasm';
const ROM_DIR = 'C:\\DEV Projects\\flycast-wasm\\demo\\roms';
const SERVER_PORT = 3001;
const SERVER_URL = `http://127.0.0.1:${SERVER_PORT}`;
const TEST_DURATION_MS = 30000;
const ROM_CLICK_TEXT = '18 Wheeler - American Pro Trucker (USA).chd';
const OUTPUT_DIR = path.join(PROJECT_DIR, 'upstream');

// Crash patterns -any of these in console = definitive crash
const CRASH_PATTERNS = [
  'Failed to start game',
  'missing function:',
  'native code called abort',
  'RuntimeError:',
  'table index is out of bounds',
  'unreachable executed',
  'Aborted(',
];

// Visual thresholds
const BLACK_SCREEN_THRESHOLD = 0.01;   // <1% non-black pixels = black screen
const VISUAL_CONTENT_THRESHOLD = 0.05; // <5% non-black pixels = no meaningful content
const UNIQUE_COLORS_THRESHOLD = 50;    // fewer than 50 unique colors = no meaningful content

// Behavior thresholds
const MAX_MAINLOOP_CYCLES = 60; // each mainloop = one frame, expect ~30fps × 30s ≈ 900

async function startServer() {
  return new Promise((resolve, reject) => {
    const server = spawn('node', ['demo/server.js', String(SERVER_PORT), ROM_DIR], {
      cwd: PROJECT_DIR,
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    let started = false;

    server.stdout.on('data', (data) => {
      const text = data.toString();
      if (!started && (text.includes('listening') || text.includes('Server') || text.includes(String(SERVER_PORT)))) {
        started = true;
        resolve(server);
      }
    });

    server.stderr.on('data', (data) => {
      console.error(`[server stderr] ${data.toString().trim()}`);
    });

    server.on('error', reject);

    // Fallback -if no "listening" message, assume ready after 3 seconds
    setTimeout(() => {
      if (!started) {
        started = true;
        resolve(server);
      }
    }, 3000);
  });
}

/**
 * Analyze a PNG screenshot for visual content.
 * Decodes actual pixel data -not compressed PNG bytes.
 *
 * Returns:
 *   totalPixels    -total pixel count
 *   nonBlackPixels -pixels where R+G+B > 30
 *   nonBlackRatio  -ratio of non-black pixels (0.0 - 1.0)
 *   uniqueColors   -count of distinct colors (quantized to 6-bit per channel)
 *   isBlack        -true if < BLACK_SCREEN_THRESHOLD non-black pixels
 *   hasVisual      -true if enough non-black pixels AND enough color diversity
 */
function analyzeScreenshot(screenshotPath) {
  const fileData = fs.readFileSync(screenshotPath);
  const png = PNG.sync.read(fileData);
  const { width, height, data } = png;
  const totalPixels = width * height;

  let nonBlackPixels = 0;
  const colorSet = new Set();

  for (let i = 0; i < data.length; i += 4) {
    const r = data[i];
    const g = data[i + 1];
    const b = data[i + 2];
    // alpha is data[i + 3]

    if (r + g + b > 30) {
      nonBlackPixels++;
    }

    // Quantize to 6-bit per channel (64 levels) to group similar colors
    const qr = r >> 2;
    const qg = g >> 2;
    const qb = b >> 2;
    colorSet.add((qr << 12) | (qg << 6) | qb);
  }

  const nonBlackRatio = nonBlackPixels / totalPixels;
  const uniqueColors = colorSet.size;
  const isBlack = nonBlackRatio < BLACK_SCREEN_THRESHOLD;
  const hasVisual = nonBlackRatio >= VISUAL_CONTENT_THRESHOLD && uniqueColors >= UNIQUE_COLORS_THRESHOLD;

  return {
    width,
    height,
    totalPixels,
    nonBlackPixels,
    nonBlackRatio: Math.round(nonBlackRatio * 10000) / 100, // percentage with 2 decimals
    uniqueColors,
    isBlack,
    hasVisual,
  };
}

/**
 * Analyze console messages for behavioral problems beyond crashes.
 *
 * Returns:
 *   mainloopEntries  -how many times "Entering mainloop" appeared
 *   mainloopExits    -how many times "Exited mainloop" appeared
 *   isCycling        -true if mainloop entered too many times (broken dispatch)
 *   warnings         -array of warning strings for non-fatal issues
 */
function analyzeConsoleBehavior(messages) {
  const warnings = [];

  const mainloopEntries = messages.filter(m => m.includes('Entering mainloop')).length;
  const mainloopExits = messages.filter(m => m.includes('Exited mainloop')).length;
  const isCycling = mainloopEntries > MAX_MAINLOOP_CYCLES;

  if (isCycling) {
    warnings.push(`Mainloop cycling: entered ${mainloopEntries} times, exited ${mainloopExits} times (max expected: ${MAX_MAINLOOP_CYCLES}). CPU is not staying in the run loop.`);
  }

  // Check for WebGL errors that might prevent rendering
  const webglErrors = messages.filter(m => m.includes('WebGL:') && (m.includes('ERROR') || m.includes('INVALID')));
  if (webglErrors.length > 3) {
    warnings.push(`${webglErrors.length} WebGL errors detected -rendering may be broken.`);
  }

  // Check for memory growth failures
  const memFails = messages.filter(m => m.includes('Cannot enlarge memory') || m.includes('OOM') || m.includes('out of memory'));
  if (memFails.length > 0) {
    warnings.push(`Memory allocation failures detected: ${memFails[0]}`);
  }

  // Check if mainloop was never entered (init failed silently)
  if (mainloopEntries === 0) {
    const hasInit = messages.some(m => m.includes('Sh4Recompiler::Init') || m.includes('Entering mainloop'));
    if (!hasInit) {
      warnings.push('Mainloop was never entered -emulation may not have started.');
    }
  }

  return {
    mainloopEntries,
    mainloopExits,
    isCycling,
    warnings,
  };
}

async function runTest() {
  const consolePath = path.join(OUTPUT_DIR, 'test-console.log');
  const screenshotPath = path.join(OUTPUT_DIR, 'test-screenshot.png');
  const resultsPath = path.join(OUTPUT_DIR, 'test-results.json');
  const contextPath = path.join(OUTPUT_DIR, 'test-context.json');

  // === HARD GATE: test-context.json must exist ===
  // This file is DELETED after each test, forcing a new one before the next run.
  // This ensures every test has documented context for notifications and history.
  if (!fs.existsSync(contextPath)) {
    console.error('');
    console.error('==========================================================');
    console.error(' TEST BLOCKED -missing upstream/test-context.json');
    console.error('==========================================================');
    console.error('');
    console.error(' Before running a test, create this file with:');
    console.error('');
    console.error(' {');
    console.error('   "issue": "What problem are we solving?",');
    console.error('   "testing": "What specific change is being tested?",');
    console.error('   "if_pass": "What does PASS mean? What is the next step?",');
    console.error('   "if_fail": "What does FAIL mean? What is the next step?"');
    console.error(' }');
    console.error('');
    console.error(' Example:');
    console.error(' {');
    console.error('   "issue": "SHIL executor renders black screen instead of BIOS",');
    console.error('   "testing": "Charging guest_cycles before SHIL ops (like x64 JIT)",');
    console.error('   "if_pass": "Cycle timing was the root cause -move to compiled WASM",');
    console.error('   "if_fail": "Try memory write trace to find SHIL vs ref divergence"');
    console.error(' }');
    console.error('');
    console.error(' Write it with: Write tool → upstream/test-context.json');
    console.error('==========================================================');
    process.exit(1);
  }

  let testContext = {};
  try {
    testContext = JSON.parse(fs.readFileSync(contextPath, 'utf-8'));
  } catch (e) {
    console.error(`[harness] Failed to parse test-context.json: ${e.message}`);
    process.exit(1);
  }

  const requiredFields = ['issue', 'testing', 'if_pass', 'if_fail'];
  const missing = requiredFields.filter(f => !testContext[f]);
  if (missing.length > 0) {
    console.error(`[harness] test-context.json missing required fields: ${missing.join(', ')}`);
    console.error('[harness] All of: issue, testing, if_pass, if_fail must be present.');
    process.exit(1);
  }

  console.log(`[context] Issue: ${testContext.issue}`);
  console.log(`[context] Testing: ${testContext.testing}`);

  // Ensure output dir exists
  fs.mkdirSync(OUTPUT_DIR, { recursive: true });

  // Clear previous results
  for (const f of [consolePath, screenshotPath, resultsPath]) {
    if (fs.existsSync(f)) fs.unlinkSync(f);
  }

  let server;
  let browser;
  const consoleMessages = [];

  try {
    // 1. Start demo server
    console.log('[harness] Starting demo server...');
    server = await startServer();
    console.log(`[harness] Server started on port ${SERVER_PORT}`);

    // 2. Launch browser (real Chromium, not headless -needs WebGL2)
    console.log('[harness] Launching browser...');
    browser = await chromium.launch({
      headless: false,
      args: [
        '--enable-webgl2-compute-context',
        '--use-gl=angle',
        '--enable-gpu',
        '--no-sandbox',
      ],
    });

    const context = await browser.newContext({
      viewport: { width: 1280, height: 720 },
    });
    const page = await context.newPage();

    // 3. Capture ALL console messages
    page.on('console', (msg) => {
      const timestamp = new Date().toISOString();
      const type = msg.type().toUpperCase();
      const text = msg.text();
      consoleMessages.push(`[${timestamp}] [${type}] ${text}`);
    });

    page.on('pageerror', (err) => {
      const timestamp = new Date().toISOString();
      consoleMessages.push(`[${timestamp}] [PAGE_ERROR] ${err.message}\n${err.stack || ''}`);
    });

    page.on('crash', () => {
      const timestamp = new Date().toISOString();
      consoleMessages.push(`[${timestamp}] [CRASH] Page crashed`);
    });

    // 4. Navigate to demo server
    console.log('[harness] Navigating to demo server...');
    await page.goto(SERVER_URL, { waitUntil: 'networkidle', timeout: 15000 });

    // 5. Click the ROM to start it
    console.log(`[harness] Clicking ROM: ${ROM_CLICK_TEXT}`);
    const romElement = await page.getByText(ROM_CLICK_TEXT, { exact: false }).first();
    if (!romElement) {
      throw new Error(`ROM not found in game list: ${ROM_CLICK_TEXT}`);
    }
    await romElement.click();

    // 6. Wait for the test duration
    console.log(`[harness] Running for ${TEST_DURATION_MS / 1000} seconds...`);
    await page.waitForTimeout(TEST_DURATION_MS);

    // 7. Take screenshot
    console.log('[harness] Taking screenshot...');
    await page.screenshot({ path: screenshotPath, fullPage: false });

    // 8. Write full console log
    fs.writeFileSync(consolePath, consoleMessages.join('\n'), 'utf-8');
    console.log(`[harness] Console log written: ${consolePath} (${consoleMessages.length} messages)`);

    // ========================================
    // 9. ANALYSIS -crash, visual, behavioral
    // ========================================

    // 9a. Crash detection
    const crashErrors = [];
    for (const pattern of CRASH_PATTERNS) {
      const matches = consoleMessages.filter(m => m.includes(pattern));
      if (matches.length > 0) {
        crashErrors.push({
          pattern,
          count: matches.length,
          first_occurrence: matches[0],
        });
      }
    }
    const crashed = crashErrors.length > 0;

    // 9b. Screenshot analysis (decoded pixel data)
    let visual = { isBlack: true, hasVisual: false, nonBlackRatio: 0, uniqueColors: 0 };
    if (fs.existsSync(screenshotPath)) {
      try {
        visual = analyzeScreenshot(screenshotPath);
      } catch (err) {
        console.error(`[harness] Screenshot analysis failed: ${err.message}`);
      }
    }

    // 9c. Behavioral analysis
    const behavior = analyzeConsoleBehavior(consoleMessages);

    // ========================================
    // 10. DETERMINE STATUS
    // ========================================
    let status;
    const failureReasons = [];

    if (crashed) {
      status = 'FAIL_CRASH';
      failureReasons.push(`CRASHED: ${crashErrors.map(e => e.pattern).join(', ')}`);
    } else if (visual.isBlack) {
      status = 'FAIL_BLACK';
      failureReasons.push(`BLACK SCREEN: ${visual.nonBlackRatio}% non-black pixels, ${visual.uniqueColors} unique colors`);
    } else if (!visual.hasVisual) {
      status = 'FAIL_NO_VISUAL';
      failureReasons.push(`NO MEANINGFUL VISUAL: ${visual.nonBlackRatio}% non-black pixels, ${visual.uniqueColors} unique colors (need >${VISUAL_CONTENT_THRESHOLD * 100}% and >${UNIQUE_COLORS_THRESHOLD} colors)`);
    } else if (behavior.isCycling) {
      status = 'FAIL_BEHAVIOR';
      failureReasons.push(`BROKEN BEHAVIOR: ${behavior.warnings.join('; ')}`);
    } else {
      status = 'PASS';
    }

    // Add behavioral warnings even on PASS (informational)
    if (behavior.warnings.length > 0 && status === 'PASS') {
      // Downgrade to FAIL_BEHAVIOR if there are warnings
      // Actually, keep PASS but include warnings in results
    }

    // ========================================
    // 11. BUILD RESULTS
    // ========================================
    const results = {
      status,
      timestamp: new Date().toISOString(),
      rom: ROM_CLICK_TEXT,
      duration_seconds: TEST_DURATION_MS / 1000,
      total_console_messages: consoleMessages.length,
      crashed,
      crash_errors: crashErrors,
      screenshot: {
        black_screen: visual.isBlack,
        has_visual_content: visual.hasVisual,
        non_black_pixels_pct: visual.nonBlackRatio,
        unique_colors: visual.uniqueColors,
        resolution: `${visual.width}x${visual.height}`,
      },
      behavior: {
        mainloop_entries: behavior.mainloopEntries,
        mainloop_exits: behavior.mainloopExits,
        mainloop_cycling: behavior.isCycling,
        warnings: behavior.warnings,
      },
      paths: {
        console_log: consolePath,
        screenshot: screenshotPath,
      },
    };

    if (failureReasons.length > 0) {
      results.failure_summary = failureReasons;
    }

    fs.writeFileSync(resultsPath, JSON.stringify(results, null, 2), 'utf-8');

    // ========================================
    // 12. OUTPUT
    // ========================================
    console.log('');
    console.log(`[harness] ===== RESULT: ${status} =====`);
    console.log(`[harness] Screenshot: ${visual.nonBlackRatio}% non-black, ${visual.uniqueColors} unique colors`);
    console.log(`[harness] Mainloop: entered ${behavior.mainloopEntries}x, exited ${behavior.mainloopExits}x`);

    if (failureReasons.length > 0) {
      console.log('[harness] Failure reasons:');
      failureReasons.forEach(r => console.log(`  - ${r}`));
    }

    if (behavior.warnings.length > 0) {
      console.log('[harness] Warnings:');
      behavior.warnings.forEach(w => console.log(`  - ${w}`));
    }

    console.log(`[harness] Results: ${resultsPath}`);
    console.log(`[harness] Console:  ${consolePath}`);
    console.log(`[harness] Screenshot: ${screenshotPath}`);

    // === AUTOMATED POST-TEST ACTIONS ===
    // These run automatically so compliance doesn't depend on agent discipline.
    // Patches are auto-regenerated by build-and-deploy.sh before build.

    // 1. AUTO-SEND ntfy notification (plain text, human-readable, with project context)
    const ntfyTitle = `Flycast WASM - ${status}`;
    const ntfyLines = [];
    // Project context (from test-context.json)
    ntfyLines.push(`Issue: ${testContext.issue}`);
    ntfyLines.push(`Tested: ${testContext.testing}`);
    ntfyLines.push('');
    // Test result
    if (status === 'PASS') {
      ntfyLines.push(`Result: BIOS rendered! ${visual.nonBlackRatio}% screen coverage, ${visual.uniqueColors} colors.`);
    } else if (status === 'FAIL_BLACK') {
      ntfyLines.push(`Result: Black screen -BIOS did not render any graphics.`);
    } else if (status === 'FAIL_CRASH') {
      ntfyLines.push(`Result: Emulator crashed.`);
      if (crashErrors.length > 0) ntfyLines.push(`Error: ${crashErrors[0].substring(0, 100)}`);
    } else if (status === 'FAIL_NO_VISUAL') {
      ntfyLines.push(`Result: Some pixels but no meaningful visual output.`);
    } else if (status === 'FAIL_BEHAVIOR') {
      ntfyLines.push(`Result: Emulation running but behavior is broken.`);
    } else {
      ntfyLines.push(`Result: Test harness error.`);
    }
    ntfyLines.push(`${behavior.mainloopExits} emulation frames in ${TEST_DURATION_MS / 1000}s.`);
    ntfyLines.push('');
    // Next step (based on result)
    ntfyLines.push(`Next: ${status === 'PASS' ? testContext.if_pass : testContext.if_fail}`);
    const ntfyBody = ntfyLines.join('\n');
    try {
      const ntfyPayload = JSON.stringify({
        topic: 'ccagent-ghostlaboratory',
        title: ntfyTitle,
        message: ntfyBody,
      });
      execSync(`curl -s -H "Content-Type: application/json" -d ${JSON.stringify(ntfyPayload)} https://ntfy.sh`, { timeout: 10000 });
      console.log(`[auto] ntfy sent: ${ntfyTitle}`);
    } catch (e) {
      console.log(`[auto] ntfy FAILED: ${e.message}`);
    }

    // DELETE context file -forces a new one before next test
    try {
      fs.unlinkSync(contextPath);
      console.log('[auto] test-context.json consumed (must write new one before next test)');
    } catch (e) {
      // ignore
    }

    // 2. AUTO-COMMIT patches + test results (every test = a snapshot)
    try {
      const commitMsg = `test: ${status} -${status === 'PASS' ? `${visual.nonBlackRatio}% pixels, ${behavior.mainloopExits} frames` : (failureReasons[0] || 'no visual output').substring(0, 80)}`;
      execSync('git add upstream/patches/ upstream/flycast-wasm-test.js upstream/link.sh upstream/build-and-deploy.sh', { cwd: PROJECT_DIR, timeout: 5000 });
      // Only commit if there are staged changes
      try {
        execSync('git diff --cached --quiet', { cwd: PROJECT_DIR, timeout: 5000 });
        console.log('[git] No changes to commit (working tree matches last commit).');
      } catch (_diffErr) {
        // diff --cached --quiet exits non-zero when there ARE changes
        execSync(`git commit -m "${commitMsg.replace(/"/g, '\\"')}"`, { cwd: PROJECT_DIR, timeout: 10000 });
        console.log(`[git] Auto-committed: ${commitMsg}`);
      }
    } catch (e) {
      console.log(`[git] Auto-commit failed: ${e.message}`);
    }

    // 3. AUTO-PUSH on PASS (working states are always pushed to remote)
    if (status === 'PASS') {
      try {
        execSync('git push', { cwd: PROJECT_DIR, timeout: 30000 });
        console.log('[git] Auto-pushed to remote (PASS state saved).');
      } catch (e) {
        console.log(`[git] Auto-push failed: ${e.message}`);
      }
    }

    // 4. REMAINING MANUAL STEPS (agent must still do these):
    console.log('');
    console.log('[checklist] Read test-screenshot.png -VISUALLY CONFIRM');
    console.log('[checklist] If FAIL: read test-console.log');

    return results;

  } catch (err) {
    console.error(`[harness] Fatal error: ${err.message}`);

    if (consoleMessages.length > 0) {
      fs.writeFileSync(consolePath, consoleMessages.join('\n'), 'utf-8');
    }

    const results = {
      status: 'ERROR',
      timestamp: new Date().toISOString(),
      rom: ROM_CLICK_TEXT,
      error: err.message,
      stack: err.stack,
      total_console_messages: consoleMessages.length,
      paths: {
        console_log: consolePath,
      },
    };

    fs.writeFileSync(resultsPath, JSON.stringify(results, null, 2), 'utf-8');
    return results;

  } finally {
    if (browser) {
      console.log('[harness] Closing browser...');
      await browser.close();
    }
    if (server) {
      console.log('[harness] Stopping server...');
      server.kill();
    }
  }
}

// Run
runTest().then((results) => {
  const exitCode = results.status === 'PASS' ? 0 : 1;
  process.exit(exitCode);
}).catch((err) => {
  console.error(`[harness] Unhandled error: ${err.message}`);
  process.exit(2);
});
