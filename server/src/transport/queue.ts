import { O_NOFOLLOW, O_RDONLY } from 'node:constants';
import { lstatSync, realpathSync } from 'node:fs';
import { lstat, link, open, readdir, realpath, rm, stat, unlink } from 'node:fs/promises';
import { join, parse, relative, resolve, sep } from 'node:path';
import { homedir, platform } from 'node:os';
import { decodeResult, encodeCommand, type WireRecord } from './protocol.js';

type QueueOptions = { pollIntervalMs?: number };
type RequestOptions = QueueOptions & { timeoutMs?: number; signal?: AbortSignal };
const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

function assertNoSymlinkAncestorsSync(path: string): void {
  const absolute = resolve(path);
  const root = parse(absolute).root;
  let current = root;
  for (const part of relative(root, absolute).split(sep).filter(Boolean)) {
    current = join(current, part);
    if (lstatSync(current).isSymbolicLink()) throw new Error(`symlink rejected: ${current}`);
  }
}

function assertDirectorySync(path: string): void {
  assertNoSymlinkAncestorsSync(path);
  const info = lstatSync(path);
  if (info.isSymbolicLink()) throw new Error(`symlink rejected: ${path}`);
  if (!info.isDirectory()) throw new Error(`not a directory: ${path}`);
  if (realpathSync(path) !== realpathSync(resolve(path))) throw new Error(`realpath mismatch: ${path}`);
}

async function assertDirectory(path: string): Promise<void> {
  const absolute = resolve(path);
  const root = parse(absolute).root;
  let current = root;
  for (const part of relative(root, absolute).split(sep).filter(Boolean)) {
    current = join(current, part);
    if ((await lstat(current)).isSymbolicLink()) throw new Error(`symlink rejected: ${current}`);
  }
  const info = await lstat(path);
  if (info.isSymbolicLink()) throw new Error(`symlink rejected: ${path}`);
  if (!info.isDirectory()) throw new Error(`not a directory: ${path}`);
  if (await realpath(path) !== await realpath(resolve(path))) throw new Error(`realpath mismatch: ${path}`);
  await stat(path);
}

async function fsyncDirectory(path: string): Promise<void> {
  const handle = await open(path, O_RDONLY);
  try { await handle.sync(); } finally { await handle.close(); }
}

async function cleanupPublication(
  tempPath: string,
  lockHandle: Awaited<ReturnType<typeof open>> | undefined,
  lockPath: string,
  ownsLock: boolean,
): Promise<void> {
  const cleanupTasks: Promise<unknown>[] = [rm(tempPath, { force: true })];
  if (lockHandle) cleanupTasks.push(lockHandle.close());
  if (ownsLock) cleanupTasks.push(rm(lockPath, { force: true }));
  await Promise.allSettled(cleanupTasks);
}

async function cleanupRequest(commandPath: string, resultPath: string): Promise<void> {
  await Promise.allSettled([
    rm(commandPath, { force: true }),
    rm(resultPath, { force: true }),
  ]);
}

async function delay(milliseconds: number, signal?: AbortSignal): Promise<void> {
  await new Promise<void>((resolveDelay, reject) => {
    let settled = false;
    let timer: NodeJS.Timeout;
    const cleanup = () => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
    };
    const finish = (fn: () => void) => {
      if (settled) return;
      settled = true;
      cleanup();
      fn();
    };
    const abort = () => finish(() => reject(new Error('request aborted')));
    timer = setTimeout(() => finish(resolveDelay), milliseconds);
    signal?.addEventListener('abort', abort, { once: true });
    if (signal?.aborted) abort();
  });
}

export function defaultQueueRoot(env: NodeJS.ProcessEnv = process.env, os = platform()): string {
  if (env.SIERRA_DATA_FOLDER?.trim()) return join(env.SIERRA_DATA_FOLDER, 'SierraMCP');
  if (os === 'win32') return join(env.LOCALAPPDATA ?? env.APPDATA ?? homedir(), 'SierraMCP');
  if (os === 'darwin') return join(env.HOME ?? homedir(), 'Library', 'Application Support', 'SierraMCP');
  return join(env.XDG_STATE_HOME ?? join(env.HOME ?? homedir(), '.local', 'state'), 'sierra-mcp');
}
export const DEFAULT_QUEUE_ROOT = defaultQueueRoot();

export class SierraQueue {
  readonly inbox: string;
  readonly outbox: string;
  private readonly pollIntervalMs: number;

  constructor(readonly root: string, options: QueueOptions = {}) {
    this.inbox = join(root, 'inbox');
    this.outbox = join(root, 'outbox');
    this.pollIntervalMs = options.pollIntervalMs ?? 25;
    assertDirectorySync(root);
    assertDirectorySync(this.inbox);
    assertDirectorySync(this.outbox);
  }

  private async validateRoot(): Promise<void> {
    await assertDirectory(this.root);
    await assertDirectory(this.inbox);
    await assertDirectory(this.outbox);
  }

  async request(record: WireRecord, options: RequestOptions = {}): Promise<WireRecord> {
    if (!uuidPattern.test(record.id)) throw new Error('invalid UUID');
    await this.validateRoot();
    const commandPath = join(this.inbox, `${record.id}.command`);
    const resultPath = join(this.outbox, `${record.id}.result`);
    const lockPath = join(this.root, `.${record.id}.lock`);
    const tempPath = join(this.inbox, `.${record.id}.${process.pid}.${Math.random().toString(16).slice(2)}.tmp`);
    let lockHandle: Awaited<ReturnType<typeof open>> | undefined;
    let ownsLock = false;

    try {
      try {
        lockHandle = await open(lockPath, 'wx', 0o600);
      } catch (error: any) {
        if (error?.code === 'EEXIST') throw new Error(`command already exists: ${record.id}`, { cause: error });
        throw error;
      }
      ownsLock = true;
      try {
        await lstat(commandPath);
        throw new Error(`command already exists: ${record.id}`);
      } catch (error: any) {
        if (error?.message?.includes('command already exists')) throw error;
        if (error?.code !== 'ENOENT') throw error;
      }
      await rm(resultPath, { force: true });
      const tempHandle = await open(tempPath, 'wx', 0o600);
      try {
        await tempHandle.writeFile(encodeCommand(record), 'utf8');
        await tempHandle.sync();
      } finally {
        await tempHandle.close();
      }
      await link(tempPath, commandPath);
      await unlink(tempPath);
      await fsyncDirectory(this.inbox);
    } finally { await cleanupPublication(tempPath, lockHandle, lockPath, ownsLock); }

    const timeoutMs = options.timeoutMs ?? 15_000;
    const deadline = Date.now() + timeoutMs;
    try {
      while (Date.now() < deadline) {
        if (options.signal?.aborted) throw new Error('request aborted');
        if ((await readdir(this.outbox)).includes(`${record.id}.result`)) {
          try {
            await assertDirectory(this.outbox);
            const resultInfo = await lstat(resultPath);
            if (!resultInfo.isFile() || resultInfo.isSymbolicLink()) throw new Error('unsafe result file');
            const resultHandle = await open(resultPath, O_RDONLY | O_NOFOLLOW);
            try {
              const response = decodeResult(await resultHandle.readFile('utf8'));
              if (response.id === record.id) return response;
            } finally {
              await resultHandle.close();
            }
          } catch { /* wait for a complete, safe, correlated result */ }
        }
        await delay(Math.min(this.pollIntervalMs, Math.max(1, deadline - Date.now())), options.signal);
      }
      throw new Error(`request timeout after ${timeoutMs}ms`);
    } finally { await cleanupRequest(commandPath, resultPath); }
  }
}
