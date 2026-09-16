import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { CallToolRequestSchema, ListToolsRequestSchema } from '@modelcontextprotocol/sdk/types.js';
import { PUBLIC_TOOLS, getToolDefinitions } from './tool-catalog.js';
import { validateToolInput, toWireFields, type PublicResult, type ToolName } from './schema.js';
import { newRequestId, type WireRecord } from './transport/protocol.js';
import { SierraQueue } from './transport/queue.js';
import { DEFAULT_QUEUE_ROOT } from './transport/queue.js';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { SierraControl } from './control.js';

const restricted = /(order|position|account|credential|trade_service|dtc|buy|sell|trade.*symbol|symbol.*trade)/i;
export function resolveQueueRoot(env: NodeJS.ProcessEnv = process.env): string { return env.SIERRA_MCP_ROOT?.trim() || DEFAULT_QUEUE_ROOT; }
export function resolveRequestTimeout(env: NodeJS.ProcessEnv = process.env): number {
  const value = Number(env.SIERRA_MCP_REQUEST_TIMEOUT_MS ?? 120_000);
  if (!Number.isInteger(value) || value < 1_000 || value > 3_600_000)
    throw new Error('invalid SIERRA_MCP_REQUEST_TIMEOUT_MS');
  return value;
}
export function getCapabilities(): { tools: ToolName[] } { return { tools: [...PUBLIC_TOOLS] }; }
function safeFields(fields: Record<string, string>): Record<string, string> { return Object.fromEntries(Object.entries(fields).filter(([key]) => !restricted.test(key))); }
function validateDispatchResponse(response: WireRecord, requestId: string): void { if (response.version !== 1 || response.id !== requestId || response.action !== 'result' || !response.fields || Object.values(response.fields).some(value => typeof value !== 'string')) throw new Error('invalid dispatcher response'); }
export function createCommandExecutor(dispatch: (record: WireRecord) => Promise<WireRecord> = async record => ({ version: 1, id: record.id, action: 'result', fields: {} })) {
  return async (name: ToolName, input: unknown): Promise<PublicResult> => {
    const parsed = validateToolInput(name, input); const requestId = newRequestId(); const bridgeInput = { ...parsed };
    if (name === 'sierra_drawings' && parsed.action === 'add') bridgeInput.drawing_id = newRequestId();
    const response = await dispatch({ version: 1, id: requestId, action: `${name}.${parsed.action}`, fields: toWireFields(bridgeInput) });
    validateDispatchResponse(response, requestId);
    const fields = safeFields(response.fields); if (bridgeInput.drawing_id && !fields.drawing_id) fields.drawing_id = bridgeInput.drawing_id as string; const changed = fields.changed === 'true';
    const chartbook = fields.chartbook;
    return { ok: fields.ok !== 'false', requestId, ...(chartbook ? { chartbook } : {}), changed, warnings: fields.warnings ? fields.warnings.split('|') : [], data: { ...fields } };
  };
}
export function createMcpServer(executor: ReturnType<typeof createCommandExecutor>, control = new SierraControl({ execute: executor })) {
  const server = new Server({ name: 'sierra-mcp', version: '1.0.0' }, { capabilities: { tools: {} } });
  server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools: getToolDefinitions() }));
  server.setRequestHandler(CallToolRequestSchema, async request => handleToolCall(request.params.name, request.params.arguments ?? {}, executor, control));
  return server;
}
export async function handleToolCall(name: string, args: unknown, executor: ReturnType<typeof createCommandExecutor>, control = new SierraControl({ execute: executor })) {
  try { if (!PUBLIC_TOOLS.includes(name as ToolName)) throw new Error('unknown tool'); const input = validateToolInput(name as ToolName, args); let result: unknown = name === 'sierra_control' ? await runControl(input, control) : await executor(name as ToolName, args); return { content: [{ type: 'text' as const, text: JSON.stringify(result) }], structuredContent: result as any }; }
  catch (error) { return makeMcpErrorResult(error); }
}
async function runControl(input: Record<string, unknown>, control: SierraControl): Promise<unknown> { switch (input.action) { case 'observe': return control.observe(); case 'health_check': return control.healthCheck(); case 'apply_profile': return control.applyProfile(input.profile as any, input.idempotency_key as string); case 'repair': return control.repair(input.profile as any, input.idempotency_key as string); case 'job_status': return control.jobStatus(input.job_id as string) ?? { id: input.job_id, state: 'failed', error: 'job not found' }; } }
async function main() {
  const queueRoot = resolveQueueRoot();
  const requestTimeoutMs = resolveRequestTimeout();
  const executor = createCommandExecutor(record => new SierraQueue(queueRoot).request(record, { timeoutMs: requestTimeoutMs }));
  const server = createMcpServer(executor);
  await server.connect(new StdioServerTransport());
}
export function makeMcpErrorResult(error: unknown): { isError: true; content: [{ type: 'text'; text: string }]; structuredContent: PublicResult } { const failure: PublicResult = { ok: false, requestId: newRequestId(), changed: false, warnings: [error instanceof Error ? error.message : String(error)], data: { error: error instanceof Error ? error.message : String(error) } }; const text=JSON.stringify(failure); return { isError: true, content: [{ type: 'text', text }], structuredContent: failure }; }
const entryPath = process.argv[1] ? resolve(process.argv[1]) : '';
if (process.argv.includes('--capabilities')) console.log(JSON.stringify(getCapabilities()));
else if (fileURLToPath(import.meta.url) === entryPath) void main();
