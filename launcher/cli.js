#!/usr/bin/env node
'use strict';
const fs = require('node:fs'); const path = require('node:path'); const { configFrom, inspect, adapter } = require('./launcher');
const args = process.argv.slice(2), command = args[0] || 'status';
function value(flag) { const i = args.indexOf(flag); return i >= 0 ? args[i + 1] : undefined; }
const file = value('--config'); let cfg = file ? JSON.parse(fs.readFileSync(path.resolve(file), 'utf8')) : {};
cfg = configFrom(cfg); if (value('--data')) cfg.dataFolder = value('--data'); if (value('--queue-root')) cfg.queueRoot = value('--queue-root'); if (value('--wine-command')) cfg.wineCommand = value('--wine-command');
if (command === 'status' || command === 'health' || command === 'watch-once') { const result = inspect(cfg); console.log(JSON.stringify(result, null, 2)); process.exitCode = command === 'health' && !result.healthy ? 1 : 0; }
else if (command === 'open') { if (!args.includes('--allow-action')) throw new Error('open requires --allow-action'); const executable = value('--executable') || cfg.sierraExecutable; adapter(cfg.platform, { wineCommand: cfg.wineCommand }).open(executable, []); }
else if (command === 'restart') throw new Error('restart is not implemented; use open with --allow-action for an explicit launch');
else { console.error('usage: status | health | watch-once | open | restart [--allow-action]'); process.exitCode = 2; }
