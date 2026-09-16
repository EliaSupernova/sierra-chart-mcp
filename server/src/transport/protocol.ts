import { randomUUID } from 'node:crypto';

export type WireRecord = { version: 1; id: string; action: string; fields: Record<string, string> };
const keyPattern = /^[A-Za-z][A-Za-z0-9_.-]{0,63}$/;
const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const reserved = new Set(['version', 'id', 'action']);

function validateRecord(record: WireRecord): void {
  if (record.version !== 1) throw new Error('unknown version');
  if (!uuidPattern.test(record.id)) throw new Error('invalid UUID');
  if (!record.action || /[\\/\x00-\x1f\x7f]/.test(record.action)) throw new Error('unsafe action');
  for (const [key, value] of Object.entries(record.fields)) {
    if (!keyPattern.test(key) || reserved.has(key)) throw new Error(`unsafe field key: ${key}`);
    if (/[\x00-\x1f\x7f]/.test(key)) throw new Error('control character');
    if (typeof value !== 'string') throw new Error('field values must be strings');
  }
}
function encode(value: string): string { return encodeURIComponent(value); }
function decode(value: string): string {
  try { return decodeURIComponent(value); } catch { throw new Error('malformed encoding'); }
}
export function encodeCommand(record: WireRecord): string { return encodeWire(record); }
export function encodeResult(record: WireRecord): string { return encodeWire(record); }
function encodeWire(record: WireRecord): string {
  validateRecord(record);
  const lines = [`version=1`, `id=${encode(record.id)}`, `action=${encode(record.action)}`];
  for (const key of Object.keys(record.fields).sort()) lines.push(`${key}=${encode(record.fields[key])}`);
  return `${lines.join('\n')}\n`;
}
export function decodeCommand(text: string): WireRecord { return decodeWire(text); }
export function decodeResult(text: string): WireRecord {
  const record = decodeWire(text);
  if (record.action !== 'result') throw new Error('invalid result action');
  return record;
}
function decodeWire(text: string): WireRecord {
  if (!text || /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(text)) throw new Error('control character');
  const lines = text.endsWith('\n') ? text.slice(0, -1).split('\n') : text.split('\n');
  const values = new Map<string, string>();
  for (const line of lines) {
    const eq = line.indexOf('=');
    if (eq <= 0 || eq !== line.lastIndexOf('=')) throw new Error('malformed line');
    const key = line.slice(0, eq);
    if (values.has(key)) throw new Error('duplicate key');
    if (key !== 'version' && key !== 'id' && key !== 'action' && (!keyPattern.test(key) || reserved.has(key))) throw new Error('unsafe field key');
    values.set(key, decode(line.slice(eq + 1)));
  }
  if (values.get('version') !== '1' || !values.has('id') || !values.has('action')) throw new Error('malformed record');
  const fields: Record<string, string> = {};
  for (const [key, value] of values) if (!reserved.has(key)) fields[key] = value;
  const record: WireRecord = { version: 1, id: values.get('id')!, action: values.get('action')!, fields };
  validateRecord(record);
  return record;
}
export function newRequestId(): string { return randomUUID(); }
