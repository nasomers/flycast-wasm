/**
 * Flycast WASM Test Harness
 * 
 * Automated test for Flycast WASM core in EmulatorJS.
 * Launches demo server, opens browser, loads ROM, captures ALL console output + screenshot.
 * 
 * Usage: node flycast-wasm-test.js
 * 
 * Prerequisites:
 *   npm install playwright
 *   ROMs in D:\Gaming\ROMs\Dreamcast\
 *   BIOS files in place for demo server
 * 
 * Output:
 *   upstream/test-results.json   — structured result with pass/fail
 *   upstream/test-console.log    — FULL raw console output (every single message)
 *   upstream/test-screenshot.png — screenshot after 30 seconds
 */

const { chromium } = require('playwright');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

// === Config ===
const PROJECT_DIR = 'C:\\DEV Projects\\flycast-wasm';
const ROM_DIR = 'D:\\Gaming\\ROMs\\Dreamcast';
const SERVER_PORT = 3001;
const SERVER_URL = `http://127.0.0.1:${SERVER_PORT}`;
const TEST_DURATION_MS = 30000;
const ROM_CLICK_TEXT = '18 Wheeler - American Pro Trucker (USA).chd';
const OUTPUT_DIR = path.join(PROJECT_DIR, 'upstream');

// These indicate a definitive failure — game did not run
const FAIL_PATTERNS = [
  'Failed to start game',
  'missing function:',
  'native code called abort',
  'RuntimeError:',
  'table index is out of bounds',
  'unreachable',
  'Aborted(',
];

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

    // Fallback — if no "listening" message, assume ready after 3 seconds
    setTimeout(() => {
      if (!started) {
        started = true;
        resolve(server);
      }
    }, 3000);
  });
}

function isBlackScreenshot(screenshotPath) {
  // Read raw PNG bytes, check if there's any pixel variance
  // Simple heuristic: sample bytes in the image data section
  // If all sampled bytes are very close to 0, it's black
  const buf = fs.readFileSync(screenshotPath);
  
  // Skip PNG header (first ~100 bytes), sample from the data
  const sampleStart = Math.min(200, buf.length);
  const sampleEnd = Math.min(buf.length, sampleStart + 10000);
  
  let nonZeroCount = 0;
  for (let i = sampleStart; i < sampleEnd; i++) {
    if (buf[i] > 10) nonZeroCount++;
  }
  
  // If less than 5% of sampled bytes are non-zero, likely black
  const ratio = nonZeroCount / (sampleEnd - sampleStart);
  return ratio < 0.05;
}

async function runTest() {
  const consolePath = path.join(OUTPUT_DIR, 'test-console.log');
  const screenshotPath = path.join(OUTPUT_DIR, 'test-screenshot.png');
  const resultsPath = path.join(OUTPUT_DIR, 'test-results.json');

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

    // 2. Launch browser (real Chromium, not headless — needs WebGL2)
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

    // 3. Capture ALL console messages — no filtering
    page.on('console', (msg) => {
      const timestamp = new Date().toISOString();
      const type = msg.type().toUpperCase();
      const text = msg.text();
      const entry = `[${timestamp}] [${type}] ${text}`;
      consoleMessages.push(entry);
    });

    // Also capture page errors
    page.on('pageerror', (err) => {
      const timestamp = new Date().toISOString();
      const entry = `[${timestamp}] [PAGE_ERROR] ${err.message}\n${err.stack || ''}`;
      consoleMessages.push(entry);
    });

    // Capture crashed/closed
    page.on('crash', () => {
      const timestamp = new Date().toISOString();
      consoleMessages.push(`[${timestamp}] [CRASH] Page crashed`);
    });

    // 4. Navigate to demo server
    console.log('[harness] Navigating to demo server...');
    await page.goto(SERVER_URL, { waitUntil: 'networkidle', timeout: 15000 });

    // 5. Click the ROM to start it
    console.log(`[harness] Clicking ROM: ${ROM_CLICK_TEXT}`);
    
    // Try to find and click the ROM link/button
    const romElement = await page.getByText(ROM_CLICK_TEXT, { exact: false }).first();
    if (!romElement) {
      throw new Error(`ROM not found in game list: ${ROM_CLICK_TEXT}`);
    }
    await romElement.click();

    // 6. Wait for the test duration, collecting console output
    console.log(`[harness] Running for ${TEST_DURATION_MS / 1000} seconds...`);
    await page.waitForTimeout(TEST_DURATION_MS);

    // 7. Take screenshot
    console.log('[harness] Taking screenshot...');
    await page.screenshot({ path: screenshotPath, fullPage: false });

    // 8. Write full console log
    fs.writeFileSync(consolePath, consoleMessages.join('\n'), 'utf-8');
    console.log(`[harness] Console log written: ${consolePath} (${consoleMessages.length} messages)`);

    // 9. Analyze results
    const fullLog = consoleMessages.join('\n');
    const errors = [];
    
    for (const pattern of FAIL_PATTERNS) {
      const matches = consoleMessages.filter(m => m.includes(pattern));
      if (matches.length > 0) {
        errors.push({
          pattern,
          count: matches.length,
          first_occurrence: matches[0],
        });
      }
    }

    const crashed = errors.length > 0;
    const blackScreen = fs.existsSync(screenshotPath) ? isBlackScreenshot(screenshotPath) : true;
    const passed = !crashed && !blackScreen;

    const results = {
      status: passed ? 'PASS' : 'FAIL',
      timestamp: new Date().toISOString(),
      rom: ROM_CLICK_TEXT,
      duration_seconds: TEST_DURATION_MS / 1000,
      total_console_messages: consoleMessages.length,
      crashed,
      black_screen: blackScreen,
      errors,
      console_log_path: consolePath,
      screenshot_path: screenshotPath,
    };

    if (!passed) {
      // Add failure summary for CC to quickly read
      results.failure_summary = [];
      if (crashed) {
        results.failure_summary.push(`CRASHED: ${errors.map(e => e.pattern).join(', ')}`);
      }
      if (blackScreen) {
        results.failure_summary.push('BLACK SCREEN: Screenshot shows no rendered content');
      }
    }

    fs.writeFileSync(resultsPath, JSON.stringify(results, null, 2), 'utf-8');
    console.log(`\n[harness] ===== RESULT: ${results.status} =====`);
    
    if (!passed) {
      console.log('[harness] Failure reasons:');
      results.failure_summary.forEach(r => console.log(`  - ${r}`));
    }

    console.log(`[harness] Results: ${resultsPath}`);
    console.log(`[harness] Console:  ${consolePath}`);
    console.log(`[harness] Screenshot: ${screenshotPath}`);

    return results;

  } catch (err) {
    console.error(`[harness] Fatal error: ${err.message}`);
    
    // Write whatever we have
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
      console_log_path: consolePath,
    };

    fs.writeFileSync(resultsPath, JSON.stringify(results, null, 2), 'utf-8');
    return results;

  } finally {
    // Cleanup
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
  process.exit(results.status === 'PASS' ? 0 : 1);
}).catch((err) => {
  console.error(`[harness] Unhandled error: ${err.message}`);
  process.exit(2);
});
