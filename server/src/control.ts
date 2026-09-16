import { newRequestId } from './transport/protocol.js';
import type { PublicResult, ToolName } from './schema.js';

export type JobState = 'accepted' | 'queued' | 'completed' | 'failed';
export type BarSessionSettings = {
  bar_period_type?: string;
  bar_period_value?: number;
  session_start?: string;
  session_end?: string;
  evening_session_start?: string;
  evening_session_end?: string;
  use_evening_session?: boolean;
};
export type DesiredChart = BarSessionSettings & { chart_number: number; symbol: string; days_to_load: number };
export type DesiredStateProfile = { chartbook: string; charts: DesiredChart[] };
export type ControlJob = { id: string; idempotencyKey: string; state: JobState; result?: PublicResult; error?: string };

export type LowLevelExecutor = (name: ToolName, input: unknown) => Promise<PublicResult>;
export type ControlDependencies = {
  execute: LowLevelExecutor;
  now?: () => string;
  id?: () => string;
};

export class JobCoordinator {
  private readonly jobs = new Map<string, ControlJob>();
  private readonly fingerprints = new Map<string, string>();
  constructor(private readonly deps: ControlDependencies) {}

  get(id: string): ControlJob | undefined { return [...this.jobs.values()].find(job => job.id === id || job.idempotencyKey === id); }

  submit(idempotencyKey: string, fingerprint: string, work: () => Promise<PublicResult>): ControlJob {
    const existing = this.jobs.get(idempotencyKey);
    if (existing) {
      if (this.fingerprints.get(idempotencyKey) !== fingerprint)
        throw new Error('idempotency key reused with different control request');
      return existing;
    }
    const job: ControlJob = { id: this.deps.id?.() ?? newRequestId(), idempotencyKey, state: 'accepted' };
    this.jobs.set(idempotencyKey, job);
    this.fingerprints.set(idempotencyKey, fingerprint);
    queueMicrotask(async () => {
      job.state = 'queued';
      try { job.result = await work(); job.state = job.result.ok ? 'completed' : 'failed'; }
      catch (error) { job.error = error instanceof Error ? error.message : String(error); job.state = 'failed'; }
    });
    return job;
  }
}

export class SierraControl {
  readonly jobs: JobCoordinator;
  constructor(private readonly deps: ControlDependencies) { this.jobs = new JobCoordinator(deps); }

  async observe(): Promise<PublicResult> {
    return this.deps.execute('sierra_status', { action: 'read' });
  }
  async healthCheck(): Promise<PublicResult> {
    const [status, diagnostics, data] = await Promise.all([
      this.deps.execute('sierra_status', { action: 'read' }),
      this.deps.execute('sierra_diagnostics', { action: 'capabilities' }),
      this.deps.execute('sierra_data', { action: 'status' }),
    ]);
    return { ok: status.ok && diagnostics.ok && data.ok, requestId: newRequestId(), changed: false, warnings: [...status.warnings, ...diagnostics.warnings, ...data.warnings], data: { status: status.data, diagnostics: diagnostics.data, data: data.data } };
  }
  applyProfile(profile: DesiredStateProfile, idempotencyKey: string): ControlJob {
    return this.jobs.submit(idempotencyKey, JSON.stringify({ action: 'apply_profile', profile }),
      () => this.convergeProfile(profile, idempotencyKey));
  }
  repair(profile: DesiredStateProfile, idempotencyKey: string): ControlJob {
    const key = `repair:${idempotencyKey}`;
    return this.jobs.submit(key, JSON.stringify({ action: 'repair', profile }),
      () => this.convergeProfile(profile, key));
  }
  jobStatus(id: string): ControlJob | undefined { return this.jobs.get(id); }

  private async convergeProfile(profile: DesiredStateProfile, idempotencyKey: string): Promise<PublicResult> {
    const observed = await this.deps.execute('sierra_status', { action: 'read' });
    if (!observed.ok) return observed;
    const currentChartbook = typeof observed.data.chartbook === 'string' ? observed.data.chartbook : observed.chartbook;
    if (!currentChartbook)
      return { ok: false, requestId: newRequestId(), changed: false, warnings: ['bridge did not report the active chartbook'], data: { code: 'chartbook_unknown' } };
    if (currentChartbook !== profile.chartbook) {
      const open = await this.deps.execute('sierra_chartbooks', { action: 'open', target_chartbook: profile.chartbook, expected_chartbook: currentChartbook, idempotency_key: `${idempotencyKey}:open` });
      if (!open.ok) return open;
      const reopened = await this.deps.execute('sierra_status', { action: 'read' });
      const active = typeof reopened.data.chartbook === 'string' ? reopened.data.chartbook : reopened.chartbook;
      if (!reopened.ok || active !== profile.chartbook)
        return { ok: false, requestId: newRequestId(), chartbook: active, changed: open.changed, warnings: [...open.warnings, ...reopened.warnings, 'target chartbook is not ready'], data: { code: 'chartbook_open_pending', target_chartbook: profile.chartbook } };
    }
    const results: PublicResult[] = [];
    for (const chart of profile.charts) {
      const target = { target_chartbook: profile.chartbook, target_chart_number: chart.chart_number, expected_chartbook: profile.chartbook, expected_chart_number: chart.chart_number };
      const symbolResult = await this.deps.execute('sierra_charts', { action: 'update', ...target, symbol: chart.symbol, idempotency_key: `${idempotencyKey}:chart:${chart.chart_number}:symbol` });
      results.push(symbolResult);
      if (!symbolResult.ok) break;
      const { chart_number: _chartNumber, symbol: _symbol, ...settings } = chart;
      results.push(await this.deps.execute('sierra_charts', { action: 'update', ...target, ...settings, idempotency_key: `${idempotencyKey}:chart:${chart.chart_number}:settings` }));
      if (!results[results.length - 1].ok) break;
    }
    return { ok: results.every(result => result.ok), requestId: newRequestId(), chartbook: profile.chartbook, changed: results.some(result => result.changed), warnings: results.flatMap(result => result.warnings), data: { charts: results.map(result => result.data), observed_chartbook: currentChartbook } };
  }
}
