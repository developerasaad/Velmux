import {create} from 'zustand';

import {isJobState} from '../constants/jobStates';
import * as engineClient from '../native/engineClient';
import {EngineBridgeUnavailable} from '../native/engineClient';
import type {ConversionRequest, JobSnapshot} from '../native/NativeVelmux';

/**
 * Job state as observed from the engine. React Native is never the source
 * of truth — snapshots come from `listJobs()` and live updates from the
 * `velmux.jobEvent` native event, coalesced to at most ~4 flushes per
 * second to keep the UI smooth.
 */

export const BRIDGE_UNAVAILABLE_MESSAGE =
  'Native engine not linked in this build';

const EVENT_FLUSH_INTERVAL_MS = 250;

export interface JobsState {
  /** Whether the native engine module is present in this build. */
  bridgeAvailable: boolean;
  /** Why the bridge is unavailable, when it is. */
  engineError: string | null;
  /** Latest snapshots in engine order (assumed oldest first). */
  jobs: JobSnapshot[];
  /** Requests for jobs started from this JS session, keyed by job id. */
  requests: Record<string, ConversionRequest>;
  /** Backend registry summary from `describeBackends()`. */
  backends: string[] | null;
  backendsError: string | null;
  initialized: boolean;
  refreshing: boolean;
  refreshError: string | null;
  init: () => Promise<void>;
  refresh: () => Promise<void>;
  discoverBackends: () => Promise<void>;
  startJob: (request: ConversionRequest) => Promise<string>;
  cancelJob: (jobId: string) => Promise<boolean>;
  pauseJob: (jobId: string) => Promise<boolean>;
  resumeJob: (jobId: string) => Promise<boolean>;
  retryJob: (jobId: string) => Promise<string | null>;
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

export const useJobsStore = create<JobsState>()((set, get) => ({
  bridgeAvailable: false,
  engineError: null,
  jobs: [],
  requests: {},
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
      set({
        bridgeAvailable: false,
        engineError: BRIDGE_UNAVAILABLE_MESSAGE,
        initialized: true,
      });
      return;
    }

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

  startJob: async request => {
    const jobId = await engineClient.startJob(request);
    set(state => ({requests: {...state.requests, [jobId]: request}}));
    await get().refresh();
    return jobId;
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
    const request = get().requests[jobId];
    if (!request) {
      return null;
    }
    return get().startJob(request);
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
