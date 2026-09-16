'use strict';
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const DEFAULT_MIN_FREE_BYTES = 1024 * 1024 * 1024;

const exists = (p, f = fs) => { try { return f.statSync(p); } catch { return null; } };
function configFrom(input = {}, env = process.env) {
  const c = { heartbeatMaxAgeSeconds: 30, scidMaxAgeSeconds: 300, minFreeBytes: DEFAULT_MIN_FREE_BYTES,
    bridgeHeartbeatFiles: ['state/bridge-heartbeat'],
    queueDirectories: ['inbox', 'outbox', 'processing', 'archive', 'state'],
    sierraProcessNames: ['SierraChart_64.exe', 'SierraChart.exe'], ...input };
  if (env.SIERRA_MCP_DATA_FOLDER) c.dataFolder = env.SIERRA_MCP_DATA_FOLDER;
  if (env.SIERRA_MCP_QUEUE_ROOT) c.queueRoot = env.SIERRA_MCP_QUEUE_ROOT;
  if (env.SIERRA_MCP_PLATFORM) c.platform = env.SIERRA_MCP_PLATFORM;
  if (env.SIERRA_MCP_HEARTBEAT_MAX_AGE) c.heartbeatMaxAgeSeconds = Number(env.SIERRA_MCP_HEARTBEAT_MAX_AGE);
  if (env.SIERRA_MCP_SCID_MAX_AGE) c.scidMaxAgeSeconds = Number(env.SIERRA_MCP_SCID_MAX_AGE);
  if (env.SIERRA_MCP_MIN_FREE_BYTES) c.minFreeBytes = Number(env.SIERRA_MCP_MIN_FREE_BYTES);
  return c;
}
function adapter(platform, deps = {}) {
  const exec = deps.execFileSync || cp.execFileSync;
  const spawn = deps.spawn || cp.spawn;
  const p = platform === 'windows' ? 'windows' : platform === 'wine' ? 'wine' : os.platform() === 'win32' ? 'windows' : 'wine';
  return { name: p, processList() { try { return p === 'windows' ? exec('tasklist', ['/FO', 'CSV', '/NH'], { encoding: 'utf8' }) : exec('ps', ['-axo', 'comm='], { encoding: 'utf8' }); } catch { return ''; } },
    open(executable, args = []) { if (!executable || /[<>]/.test(executable)) throw new Error('an explicit executable is required'); const command = p === 'wine' ? deps.wineCommand || 'wine' : executable; const commandArgs = p === 'wine' ? [executable, ...args] : args; const child = spawn(command, commandArgs, { detached: true, stdio: 'ignore', cwd: path.dirname(executable) }); child.unref(); return child.pid; } };
}
function inspect(cfg, deps = {}) {
  const f = deps.fs || fs, now = deps.now || Date.now, data = cfg.dataFolder, root = cfg.queueRoot || (data ? path.join(data, 'SierraMCP') : undefined);
  const ad = deps.adapter || adapter(cfg.platform, deps), processes = ad.processList();
  const names = cfg.sierraProcessNames || ['SierraChart_64.exe', 'SierraChart.exe'];
  const process = names.some(n => processes.toLowerCase().includes(n.toLowerCase()));
  const queues = Object.fromEntries((cfg.queueDirectories || []).map(n => [n, !!(root && exists(path.join(root, n), f)?.isDirectory())]));
  const hearts = (cfg.bridgeHeartbeatFiles || ['state/bridge-heartbeat']).map(n => path.isAbsolute(n) ? n : path.join(root || '', n));
  const heartbeatPath = hearts.find(p => exists(p, f)?.isFile());
  const heartbeatAgeSeconds = heartbeatPath ? Math.max(0, (now() - exists(heartbeatPath, f).mtimeMs) / 1000) : null;
  const scids = [];
  function walk(dir) { if (!dir || !exists(dir, f)?.isDirectory()) return; let entries; try { entries = f.readdirSync(dir, { withFileTypes: true }); } catch { return; } for (const e of entries) { const q = path.join(dir, e.name); if (e.isDirectory()) walk(q); else if (/\.scid$/i.test(e.name)) { try { scids.push({ path: q, ageSeconds: (now() - f.statSync(q).mtimeMs) / 1000 }); } catch {} } } }
  walk(data);
  let freeBytes = null; try { const stat = f.statfsSync(data); freeBytes = stat.bavail * stat.bsize; } catch {}
  const minFreeBytes = cfg.minFreeBytes ?? DEFAULT_MIN_FREE_BYTES;
  const staleScid = scids.length > 0 && scids.every(x => x.ageSeconds > (cfg.scidMaxAgeSeconds ?? 300));
  return { adapter: ad.name, process, queues, heartbeat: { path: heartbeatPath || null, ageSeconds: heartbeatAgeSeconds, fresh: heartbeatAgeSeconds !== null && heartbeatAgeSeconds <= (cfg.heartbeatMaxAgeSeconds ?? 30) }, dataFolder: { path: data || null, freeBytes, minFreeBytes, enoughFree: freeBytes !== null && freeBytes >= minFreeBytes }, scid: { files: scids, stale: staleScid }, healthy: process && Object.values(queues).every(Boolean) && heartbeatAgeSeconds !== null && heartbeatAgeSeconds <= (cfg.heartbeatMaxAgeSeconds ?? 30) && !staleScid && freeBytes !== null && freeBytes >= minFreeBytes };
}
module.exports = { adapter, configFrom, inspect };
