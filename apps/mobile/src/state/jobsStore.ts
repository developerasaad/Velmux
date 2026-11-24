import {create} from 'zustand';

import {isJobState, isTerminalJob} from '../constants/jobStates';
import * as engineClient from '../native/engineClient';
import {EngineBridgeUnavailable} from '../native/engineClient';
import type {
  ConversionRequest,
  JobSnapshot,
  MediaSource,
} from '../native/NativeVelmux';

/**
 * Job state as observed from the engine. React Native is never the source
 * of truth — snapshots come from `listJobs()` and live updates from the
 * `velmux.jobEvent` native event, coalesced to at most ~4 flushes per
 * second to keep the UI smooth.
 */

export const BRIDGE_UNAVAILABLE_MESSAGE =
  'Native engine not linked in this build';

const EVENT_FLUSH_INTERVAL_MS = 250;

/**
 * A multi-file batch staged by Home's multi-select flow. The workspace
 * opens with the primary file; when Start is pressed, one job is created
 * per picked file with the same settings (source swapped per file).
 */
export interface PendingBatch {
  /** URI of the file the conversion workspace opens with (first pick). */
  primaryUri: string;
  /** Remaining picks; each becomes its own job with identical settings. */
  extraSources: MediaSource[];
}

/** Per-file outcome of the most recent batch start, for Home feedback. */
export interface BatchStartOutcome {
  /** Number of picked files in the batch. */
  total: number;
  /** Number of jobs successfully handed to the engine. */
  started: number;
  /** Files that could not start, with the engine's reason. */
  failures: {name: string; reason: string}[];
}

export interface JobsState {
  /** Whether the native engine module is present in this build. */
  bridgeAvailable: boolean;
  /** Why the bridge is unavailable, when it is. */
  engineError: string | null;
  /** Latest snapshots in engine order (assumed oldest first). */
  jobs: JobSnapshot[];
  /** Requests for jobs started from this JS session, keyed by job id. */
  requests: Record<string, ConversionRequest>;
  /** Batch staged by Home multi-select, consumed by the next `startJob`. */
  pendingBatch: PendingBatch | null;
  /** Outcome of the most recent batch start; Home consumes it once. */
  lastBatchStart: BatchStartOutcome | null;
  /** Backend registry summary from `describeBackends()`. */
  backends: string[] | null;
  backendsError: string | null;
  initialized: boolean;
  refreshing: boolean;
  refreshError: string | null;
  init: () => Promise<void>;
  refresh: () => Promise<void>;
  discoverBackends: () => Promise<void>;
  /**
   * Stages a multi-file batch (Home "Convert N files"). The conversion
   * workspace still opens with a single primary source; the extras are
   * started as their own jobs by the next `startJob` from that workspace.
   */
  beginBatch: (primary: MediaSource, extras: MediaSource[]) => void;
  /** Drops any staged batch (single-file flow taking over, or reuse). */
  clearBatch: () => void;
  /** Returns (and clears) the last batch outcome, for one-shot feedback. */
  consumeBatchStart: () => BatchStartOutcome | null;
  startJob: (request: ConversionRequest) => Promise<string>;
  cancelJob: (jobId: string) => Promise<boolean>;
  pauseJob: (jobId: string) => Promise<boolean>;
  resumeJob: (jobId: string) => Promise<boolean>;
  retryJob: (jobId: string) => Promise<string | null>;
  deleteJob: (jobId: string) => Promise<boolean>;
  /** Internal: applied by the event coalescer. */
  applyEvents: (
    events: Map<string, engineClient.JobEventPayload>,
    unknownIds: Set<string>,
  ) => void;
}

/**
 * Event coalescing buffer. Job events can arrive at high frequency; we keep
 * only the latest payload per job and flush at most every 250 ms.
 */
let pendingEvents = new Map<string, engineClient.JobEventPayload>();
let flushTimer: ReturnType<typeof setTimeout> | null = null;
let unknownJobIds = new Set<string>();
let refreshScheduled = false;

function scheduleFlush() {
  if (flushTimer !== null) {
    return;
  }
  flushTimer = setTimeout(() => {
    flushTimer = null;
    flushPendingEvents();
  }, EVENT_FLUSH_INTERVAL_MS);
}

function flushPendingEvents() {
  if (pendingEvents.size === 0) {
    return;
  }
  const events = pendingEvents;
  const unknown = unknownJobIds;
  pendingEvents = new Map();
  unknownJobIds = new Set();

  useJobsStore.getState().applyEvents(events, unknown);
}

function handleJobEvent(payload: engineClient.JobEventPayload) {
  if (!isJobState(payload.state)) {
    // Unknown (future) state: resync from the engine rather than guessing.
    scheduleRefresh();
    return;
  }
  pendingEvents.set(payload.jobId, payload);
  const known = useJobsStore
    .getState()
    .jobs.some(job => job.id === payload.jobId);
  if (!known) {
    unknownJobIds.add(payload.jobId);
  }
  scheduleFlush();
}

function scheduleRefresh() {
  if (refreshScheduled) {
    return;
  }
  refreshScheduled = true;
  setTimeout(() => {
    refreshScheduled = false;
    useJobsStore.getState().refresh();
  }, EVENT_FLUSH_INTERVAL_MS);
}

let initStarted = false;

/**
 * Clones a computed request for another source of the same batch: every
 * setting carries over unchanged, only the source (and the engine-derived
 * output name) differs per file. Requests are JSON-shaped by the bridge
 * contract, so a JSON round-trip is a faithful deep copy and keeps the
 * per-file records independent for retry/reuse.
 */
function withSource(
  request: ConversionRequest,
  source: MediaSource,
): ConversionRequest {
  const clone = JSON.parse(JSON.stringify(request)) as ConversionRequest;
  clone.source = source;
  return clone;
}

export const useJobsStore = create<JobsState>()((set, get) => ({
  bridgeAvailable: false,
  engineError: null,
  jobs: [],
  requests: {},
  pendingBatch: null,
  lastBatchStart: null,
  backends: null,
  backendsError: null,
  initialized: false,
  refreshing: false,
  refreshError: null,

  init: async () => {
    if (initStarted) {
      return;
    }
    initStarted = true;

    if (!engineClient.isEngineAvailable()) {
      // Permanent startup marker: proves the JS runtime reached app init and
      // reports the bridge state. Appears in logcat / the unified log, so
      // automated runs (and field diagnostics) can verify RN initialization.
      console.info('[Velmux] app init: native bridge unavailable');
      set({
        bridgeAvailable: false,
        engineError: BRIDGE_UNAVAILABLE_MESSAGE,
        initialized: true,
      });
      return;
    }

    console.info('[Velmux] app init: native bridge available');
    set({bridgeAvailable: true, engineError: null});
    // The subscription intentionally lives for the whole JS session; job
    // events must flow regardless of which screen is mounted.
    engineClient.subscribeToJobEvents(handleJobEvent);
    await get().refresh();
    set({initialized: true});
  },

  refresh: async () => {
    if (!engineClient.isEngineAvailable()) {
      set({
        bridgeAvailable: false,
        engineError: BRIDGE_UNAVAILABLE_MESSAGE,
        refreshing: false,
        initialized: true,
      });
      return;
    }
    set({refreshing: true, refreshError: null});
    try {
      const jobs = await engineClient.listJobs();
      set({jobs, refreshing: false, refreshError: null});
    } catch (error) {
      const unavailable = error instanceof EngineBridgeUnavailable;
      set({
        refreshing: false,
        refreshError: engineClient.describeEngineError(error),
        bridgeAvailable: !unavailable,
        engineError: unavailable ? BRIDGE_UNAVAILABLE_MESSAGE : null,
      });
    }
  },

  discoverBackends: async () => {
    if (!engineClient.isEngineAvailable()) {
      set({
        backends: null,
        backendsError: BRIDGE_UNAVAILABLE_MESSAGE,
        bridgeAvailable: false,
        engineError: BRIDGE_UNAVAILABLE_MESSAGE,
      });
      return;
    }
    set({backendsError: null});
    try {
      const backends = await engineClient.describeBackends();
      set({backends, backendsError: null, bridgeAvailable: true});
    } catch (error) {
      set({
        backends: null,
        backendsError: engineClient.describeEngineError(error),
      });
    }
  },

  beginBatch: (primary, extras) => {
    set({
      pendingBatch:
        extras.length > 0
          ? {primaryUri: primary.uri, extraSources: extras}
          : null,
    });
  },

  clearBatch: () => {
    set({pendingBatch: null});
  },

  consumeBatchStart: () => {
    const outcome = get().lastBatchStart;
    if (outcome !== null) {
      set({lastBatchStart: null});
    }
    return outcome;
  },

  startJob: async request => {
    const batch = get().pendingBatch;
    const batchApplies =
      batch !== null &&
      batch.primaryUri === request.source.uri &&
      batch.extraSources.length > 0;
    if (batch !== null && !batchApplies) {
      // Stale batch: the user re-picked or entered the workspace another
      // way. It must not leak into an unrelated conversion.
      set({pendingBatch: null});
    }
    if (!batchApplies) {
      const jobId = await engineClient.startJob(request);
      set(state => ({requests: {...state.requests, [jobId]: request}}));
      await get().refresh();
      return jobId;
    }

    // Batch start: one job per picked file, same settings, source swapped
    // per file. Sequential awaits are fine — the engine's scheduler owns
    // real concurrency and resource caps. A file that fails to start (for
    // example an unreadable source) never aborts the rest; failures are
    // collected and surfaced by Home once the workspace closes.
    const sources = [request.source, ...batch.extraSources];
    const startedIds: string[] = [];
    const failures: {name: string; reason: string}[] = [];
    for (const source of sources) {
      const perFile: ConversionRequest =
        source === request.source ? request : withSource(request, source);
      try {
        const jobId = await engineClient.startJob(perFile);
        set(state => ({requests: {...state.requests, [jobId]: perFile}}));
        startedIds.push(jobId);
      } catch (error) {
        failures.push({
          name: source.displayName,
          reason: engineClient.describeEngineError(error),
        });
      }
    }
    if (startedIds.length === 0) {
      // Total failure: rethrow so the workspace shows the engine errors and
      // the user can retry. The batch stays staged so a retry resubmits all
      // files, not just the primary one.
      throw new Error(
        failures.map(f => `${f.name}: ${f.reason}`).join('\n'),
      );
    }
    set({
      pendingBatch: null,
      lastBatchStart: {
        total: sources.length,
        started: startedIds.length,
        failures,
      },
    });
    await get().refresh();
    // The workspace treats the promise as "the flow started"; the primary
    // file's job id keeps that contract.
    return startedIds[0];
  },

  cancelJob: async jobId => {
    const canceled = await engineClient.cancelJob(jobId);
    await get().refresh();
    return canceled;
  },

  pauseJob: async jobId => {
    const paused = await engineClient.pauseJob(jobId);
    await get().refresh();
    return paused;
  },

  resumeJob: async jobId => {
    const resumed = await engineClient.resumeJob(jobId);
    await get().refresh();
    return resumed;
  },

  retryJob: async jobId => {
    // Durable engine-side retry: the original request lives in the C++
    // store, so retries work across app restarts (not just for jobs
    // started in this JS session). Falls back to a fresh submit when the
    // engine no longer knows the job (record already deleted).
    const retried = await engineClient.retryJob(jobId);
    if (!retried) {
      const request = get().requests[jobId];
      if (!request) {
        return null;
      }
      // A retry is a single-file intent; it must never piggyback on a
      // staged Home batch that happens to share the primary source URI.
      get().clearBatch();
      return get().startJob(request);
    }
    await get().refresh();
    return jobId;
  },

  deleteJob: async jobId => {
    const deleted = await engineClient.deleteJob(jobId);
    if (deleted) {
      set(state => {
        const requests = {...state.requests};
        delete requests[jobId];
        return {
          jobs: state.jobs.filter(job => job.id !== jobId),
          requests,
        };
      });
    }
    return deleted;
  },

  applyEvents: (events, unknownIds) => {
    set(state => ({
      jobs: state.jobs.map(job => {
        const event = events.get(job.id);
        if (!event || !isJobState(event.state)) {
          return job;
        }
        const updated: JobSnapshot = {
          ...job,
          state: event.state,
          progress: event.progress,
        };
        if (event.outputSizeBytes !== undefined) {
          updated.outputSizeBytes = event.outputSizeBytes;
        }
        return updated;
      }),
    }));
    if (unknownIds.size > 0) {
      scheduleRefresh();
      return;
    }
    // Terminal transitions need a full snapshot refresh: job events carry
    // state/progress/output size but not the output location, which
    // History's share/delete actions read from the snapshot.
    for (const event of events.values()) {
      if (isJobState(event.state) && isTerminalJob(event.state)) {
        scheduleRefresh();
        return;
      }
    }
  },
}));

/** Jobs the user expects to be making progress (Home "Active jobs"). */
export function selectActiveJobs(state: Pick<JobsState, 'jobs'>): JobSnapshot[] {
  return state.jobs.filter(
    job =>
      job.state !== 'completed' &&
      job.state !== 'failed' &&
      job.state !== 'canceled',
  );
}

/** Completed jobs, newest first (engine order is assumed oldest first). */
export function selectCompletedJobs(
  state: Pick<JobsState, 'jobs'>,
): JobSnapshot[] {
  return state.jobs.filter(job => job.state === 'completed').reverse();
}
