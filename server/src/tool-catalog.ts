import { toolInputSchemas, type ToolName } from './schema.js';
export const PUBLIC_TOOLS: ToolName[] = ['sierra_status', 'sierra_chartbooks', 'sierra_charts', 'sierra_studies', 'sierra_drawings', 'sierra_data', 'sierra_diagnostics', 'sierra_control'];
const descriptions: Record<ToolName, string> = {
  sierra_status: 'Read the current Sierra runtime status.', sierra_chartbooks: 'Inspect or safely change chartbook files.', sierra_charts: 'Inspect, configure, or close charts.', sierra_studies: 'Inspect or configure studies owned by this MCP.', sierra_drawings: 'Inspect or configure drawings owned by this MCP.', sierra_data: 'Read data status or request a chart refresh.', sierra_diagnostics: 'Read Sierra diagnostic logs.', sierra_control: 'Bounded high-level observation, health, profile, repair, and job control.'
};
export function getToolDefinitions() { return PUBLIC_TOOLS.map(name => { const inputSchema = toolInputSchemas[name].toJSONSchema() as Record<string, unknown>; return { name, description: descriptions[name], inputSchema: { ...inputSchema, type: 'object' } }; }); }
