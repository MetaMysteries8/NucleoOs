// Build/test the actual native core and actual vendored Lua, without ESP-IDF or mocks.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readdirSync, mkdtempSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
const root = resolve(import.meta.dirname, '../..');
const core = resolve(root, 'firmware/components/nucleo_pico8');
const build = resolve(root, 'build/pico8-host');
mkdirSync(build, { recursive: true });
const excluded = new Set(['linit.c', 'liolib.c', 'loadlib.c', 'loslib.c', 'ldblib.c', 'lcorolib.c', 'lutf8lib.c']);
const sources = readdirSync(`${core}/vendor/lua`).filter(n => n.endsWith('.c') && !excluded.has(n)).map(n => `${core}/vendor/lua/${n}`);
const exe = resolve(build, 'pico8-check');
const flags = process.env.SANITIZE === '1' ? ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] : ['-O2'];
function run(cmd, args) {
  const r = spawnSync(cmd, args, { cwd: root, stdio: 'inherit', timeout: 120000 });
  if (r.error) throw r.error;
  if (r.status !== 0) process.exit(r.status ?? 1);
}
run(process.env.CC || 'gcc', ['-std=gnu11', '-DLUAI_MAXCCALLS=40', '-DLUA_32BITS=1', '-Wall', '-Wextra', '-Werror', ...flags,
  `-I${core}/include`, `-I${core}/vendor/lua`, `${core}/nucleo_pico8.c`, ...sources,
  resolve(root, 'tools/pico8-host/test.c'), '-lm', '-o', exe]);
const scratch = mkdtempSync(resolve(tmpdir(), 'nucleo-pico8-'));
try { run(exe, [scratch, `${core}/demo/pocket-demo.p8`, resolve(build, 'demo.ppm')]); }
finally { rmSync(scratch, { recursive: true, force: true }); }
