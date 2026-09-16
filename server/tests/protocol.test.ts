import { describe, expect, it } from 'vitest';
import { decodeCommand, decodeResult, encodeCommand } from '../src/transport/protocol.js';
const id = '123e4567-e89b-12d3-a456-426614174000';
describe('wire protocol', () => {
  it('round-trips Unicode and percent values', () => { const r = { version: 1 as const, id, action: 'chart.read', fields: { name: 'café 東京\n100%' } }; expect(decodeCommand(encodeCommand(r))).toEqual(r); });
  it.each([['version=2\nid='+id+'\naction=x', 'version'], ['version=1\nid='+id+'\nid=x\naction=x', 'duplicate'], ['version=1\nid=no\naction=x', 'UUID'], ['version=1\nid='+id+'\naction=x\na=%ZZ', 'encoding'], ['version=1\nid='+id+'\naction=x\na/me=x', 'key'], ['version=1\nid='+id+'\naction=x\na=\u0000', 'control']])('rejects unsafe wire (%s)', wire => expect(() => decodeCommand(wire)).toThrow());
  it('rejects unsafe and reserved field keys on encode', () => { for (const key of ['field/key', 'version', 'bad key']) expect(() => encodeCommand({ version: 1, id, action: 'x', fields: { [key]: 'x' } })).toThrow(); });
  it('rejects a command-shaped record in the result decoder', () => { expect(() => decodeResult(encodeCommand({ version: 1, id, action: 'status.read', fields: {} }))).toThrow(/result action/); expect(decodeCommand(encodeCommand({ version: 1, id, action: 'status.read', fields: {} })).action).toBe('status.read'); });
});
