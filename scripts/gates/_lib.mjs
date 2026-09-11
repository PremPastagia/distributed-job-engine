// Shared helpers for the acceptance-gate checks.
// Every check performs its own assertions, exits non-zero on any failure, and prints its
// success marker only after all of them pass.
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
export const BUILD = path.join(ROOT, 'build');

const failures = [];

export function check(condition, message) {
  if (!condition) failures.push(message);
  return condition;
}

export function checkEq(actual, expected, message) {
  return check(actual === expected, `${message} (expected ${expected}, got ${actual})`);
}

export function fatal(message) {
  console.error(`FATAL: ${message}`);
  process.exit(1);
}

export function finish(gateId, summaryLines = []) {
  for (const line of summaryLines) console.log(line);
  if (failures.length > 0) {
    console.error(`\n${gateId} FAILED with ${failures.length} problem(s):`);
    for (const f of failures) console.error(`  - ${f}`);
    process.exit(1);
  }
  console.log(`GATE ${gateId} PASS`);
  process.exit(0);
}

export function run(cmd, args, opts = {}) {
  const res = spawnSync(cmd, args, {
    cwd: opts.cwd || ROOT,
    encoding: 'utf8',
    maxBuffer: 256 * 1024 * 1024,
    timeout: opts.timeout || 900000,
    env: { ...process.env, ...(opts.env || {}) },
  });
  const stdout = res.stdout || '';
  const stderr = res.stderr || '';
  return { code: res.status, out: stdout + stderr, stdout, stderr, error: res.error };
}

export function mustRun(cmd, args, opts = {}) {
  const r = run(cmd, args, opts);
  if (r.code !== 0) {
    console.error(`command failed (${r.code}): ${cmd} ${args.join(' ')}`);
    console.error(r.out.slice(-4000));
    if (!opts.tolerate) process.exit(1);
  }
  return r;
}

export function exists(relPath) {
  return fs.existsSync(path.join(ROOT, relPath));
}

export function readText(relPath) {
  return fs.readFileSync(path.join(ROOT, relPath), 'utf8');
}

export function ensureBuild(dir = 'build', extraArgs = []) {
  const buildDir = path.join(ROOT, dir);
  if (!fs.existsSync(path.join(buildDir, 'CMakeCache.txt'))) {
    mustRun('cmake', ['-S', ROOT, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release',
                      '-DJOBENGINE_BUILD_TESTS=ON', ...extraArgs]);
  }
  mustRun('cmake', ['--build', buildDir, '-j', String(cpuCount())]);
  return buildDir;
}

export function cpuCount() {
  return Math.max(2, os.cpus().length);
}

// ---- child process orchestration -------------------------------------------------------

export class Proc {
  constructor(cmd, args, opts = {}) {
    this.cmd = cmd;
    this.args = args;
    this.stdout = '';
    this.stderr = '';
    this.exited = false;
    this.exitCode = null;
    this.signal = null;
    this.child = spawn(cmd, args, { cwd: opts.cwd || ROOT, env: { ...process.env, ...(opts.env || {}) } });
    // Keep the head (where the READY line lives) and a bounded tail, so a long-running
    // child cannot grow this process's memory without limit.
    const cap = (current, chunk) => {
      const next = current + chunk;
      if (next.length <= 48 * 1024) return next;
      return next.slice(0, 8 * 1024) + '\n...[trimmed]...\n' + next.slice(-32 * 1024);
    };
    this.child.stdout.on('data', (d) => { this.stdout = cap(this.stdout, d.toString()); });
    this.child.stderr.on('data', (d) => { this.stderr = cap(this.stderr, d.toString()); });
    this.child.on('exit', (code, signal) => {
      this.exited = true;
      this.exitCode = code;
      this.signal = signal;
    });
  }
  get output() { return this.stdout + this.stderr; }
  kill(sig = 'SIGTERM') { if (!this.exited) { try { this.child.kill(sig); } catch {} } }
  async waitExit(timeoutMs = 15000) {
    const deadline = Date.now() + timeoutMs;
    while (!this.exited && Date.now() < deadline) await sleep(20);
    return this.exited;
  }
}

export function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

// Waits for a line matching `re` in the process output. Returns the match or null.
export async function waitForLine(proc, re, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const m = proc.output.match(re);
    if (m) return m;
    if (proc.exited) return proc.output.match(re);
    await sleep(20);
  }
  return null;
}

// Starts jobengine-server on an ephemeral port and returns { proc, port }.
export async function startServer(extraArgs = [], buildDir = BUILD) {
  const proc = new Proc(path.join(buildDir, 'jobengine-server'),
                        ['--port', '0', '--log-level', 'warn', ...extraArgs]);
  const m = await waitForLine(proc, /READY host=\S+ port=(\d+)/);
  if (!m) {
    proc.kill('SIGKILL');
    fatal(`server did not become ready. Output:\n${proc.output.slice(0, 4000)}`);
  }
  return { proc, port: Number(m[1]) };
}

export async function stopServer(server, timeoutMs = 20000) {
  server.proc.kill('SIGTERM');
  const ok = await server.proc.waitExit(timeoutMs);
  if (!ok) server.proc.kill('SIGKILL');
  return ok;
}

// ---- minimal HTTP client (no dependencies) -----------------------------------------------

export function http(port, method, reqPath, body = null, timeoutMs = 20000) {
  return new Promise((resolve) => {
    const payload = body === null ? '' : (typeof body === 'string' ? body : JSON.stringify(body));
    const socket = net.connect({ host: '127.0.0.1', port });
    let raw = '';
    let settled = false;
    const done = (result) => { if (!settled) { settled = true; socket.destroy(); resolve(result); } };
    const timer = setTimeout(() => done({ ok: false, error: 'timeout' }), timeoutMs);
    socket.on('connect', () => {
      socket.write(
        `${method} ${reqPath} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\n` +
        `Content-Type: application/json\r\nContent-Length: ${Buffer.byteLength(payload)}\r\n` +
        `Connection: close\r\n\r\n${payload}`);
    });
    socket.on('data', (d) => { raw += d.toString(); });
    socket.on('error', (e) => { clearTimeout(timer); done({ ok: false, error: e.message }); });
    socket.on('close', () => {
      clearTimeout(timer);
      const idx = raw.indexOf('\r\n\r\n');
      if (idx < 0) return done({ ok: false, error: 'no complete response', raw });
      const head = raw.slice(0, idx);
      const bodyText = raw.slice(idx + 4);
      const status = Number((head.split('\r\n')[0] || '').split(' ')[1] || 0);
      let json = null;
      try { json = JSON.parse(bodyText); } catch {}
      done({ ok: true, status, body: bodyText, json });
    });
  });
}

export async function waitFor(predicate, timeoutMs = 20000, pollMs = 50) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await predicate()) return true;
    await sleep(pollMs);
  }
  return await predicate();
}

// ---- gtest helpers -------------------------------------------------------------------------

// Runs the test binary and reports counts read from the runner's own output, so the
// numbers in a report are never hand-written.
export function runGtest(filter = '*', buildDir = BUILD, env = {}, extraArgs = []) {
  const r = run(path.join(buildDir, 'jobengine_tests'),
                [`--gtest_filter=${filter}`, '--gtest_brief=1', ...extraArgs],
                { env, timeout: 1800000 });
  const ran = /(\d+) tests? from (\d+) test suites? ran/.exec(r.out);
  const passed = /\[\s+PASSED\s+\] (\d+) tests?/.exec(r.out);
  const failed = /\[\s+FAILED\s+\] (\d+) tests?, listed below/.exec(r.out);
  return {
    ...r,
    total: ran ? Number(ran[1]) : 0,
    suites: ran ? Number(ran[2]) : 0,
    passed: passed ? Number(passed[1]) : 0,
    failed: failed ? Number(failed[1]) : 0,
  };
}

export function assertGtest(result, label, minTests = 1) {
  check(result.code === 0, `${label}: test binary exited ${result.code}`);
  check(result.total >= minTests, `${label}: expected at least ${minTests} tests, ran ${result.total}`);
  checkEq(result.passed, result.total, `${label}: not every test passed`);
  checkEq(result.failed, 0, `${label}: failing tests reported`);
  return result;
}

// ---- CSV -----------------------------------------------------------------------------------

// Splits one CSV line, honouring RFC 4180 quoting. A naive split on ',' would corrupt any
// row whose label or payload legitimately contains a comma.
function splitCsvLine(line) {
  const out = [];
  let field = '';
  let inQuotes = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (inQuotes) {
      if (c === '"') {
        if (line[i + 1] === '"') { field += '"'; i++; }
        else inQuotes = false;
      } else {
        field += c;
      }
    } else if (c === '"' && field.length === 0) {
      // A quote only opens a quoted field at the start of one. A quote appearing mid-field
      // is literal data (a JSON payload written without quoting), which is how Python's
      // csv module reads it too, so both readers agree on the same file.
      inQuotes = true;
    } else if (c === ',') {
      out.push(field);
      field = '';
    } else {
      field += c;
    }
  }
  out.push(field);
  return out;
}

export function parseCsv(text) {
  // Tolerate CRLF: a file written by another tool should not shift every trailing field.
  const lines = text.split('\n').map((l) => (l.endsWith('\r') ? l.slice(0, -1) : l))
                    .filter((l) => l.length > 0 && !l.startsWith('#'));
  if (lines.length === 0) return { header: [], rows: [] };
  const header = splitCsvLine(lines[0]);
  const rows = lines.slice(1).map((l) => {
    const parts = splitCsvLine(l);
    const obj = {};
    header.forEach((h, i) => { obj[h] = parts[i]; });
    return obj;
  });
  return { header, rows };
}

export function csvComments(text) {
  return text.split('\n').filter((l) => l.startsWith('#'));
}

// Nearest-rank percentile, matching apps/bench_common.hpp so offline and in-process
// numbers are computed the same way.
export function percentile(sortedNumbers, p) {
  if (sortedNumbers.length === 0) return 0;
  let idx = Math.ceil((p / 100) * sortedNumbers.length);
  if (idx < 1) idx = 1;
  if (idx > sortedNumbers.length) idx = sortedNumbers.length;
  return sortedNumbers[idx - 1];
}
