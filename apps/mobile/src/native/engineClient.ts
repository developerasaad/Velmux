import {DeviceEventEmitter, TurboModuleRegistry} from 'react-native';

import type {
  ConversionRequest,
  JobSnapshot,
  MediaInfo,
  MediaSource,
  ResourceUpdate,
  Spec,
} from './NativeVelmux';

/**
 * Typed client for the Velmux native engine (TurboModule `VelmuxEngine`).
 *
 * The native engine is owned by the C++ core and linked by the lead in a
 * later milestone. Until the module is registered, this client reports
 * unavailability as a first-class condition: `isEngineAvailable()` is the
 * sync check and every call throws `EngineBridgeUnavailable`.
 *
 * UI code must import from this module only — never from `./NativeVelmux`
 * directly, whose default export is enforcing and would crash at import
 * time when the bridge is missing.
 */

export const ENGINE_MODULE_NAME = 'VelmuxEngine';
export const JOB_EVENT_NAME = 'velmux.jobEvent';

export interface JobEventPayload {
  jobId: string;
  state: string;
  progress: number;
  outputSizeBytes?: number | null;
}

export class EngineBridgeUnavailable extends Error {
  constructor(message = 'Native engine not linked in this build') {
    super(message);
    this.name = 'EngineBridgeUnavailable';
  }
}

let cachedModule: Spec | null | undefined;

function getModule(): Spec | null {
  if (cachedModule === undefined) {
    try {
      cachedModule = TurboModuleRegistry.get<Spec>(ENGINE_MODULE_NAME) ?? null;
    } catch {
      cachedModule = null;
    }
  }
  return cachedModule;
}

/** Sync availability check. Safe to call at any time, including at startup. */
export function isEngineAvailable(): boolean {
  return getModule() !== null;
}

function requireModule(): Spec {
  const module = getModule();
  if (!module) {
    throw new EngineBridgeUnavailable();
  }
  return module;
}

export function inspect(source: MediaSource): Promise<MediaInfo> {
  return requireModule().inspect(source);
}

export function plan(request: ConversionRequest): Promise<{
  estimatedOutputBytes: number | null;
  pipelineSummary: string;
  feasible: boolean;
  message: string | null;
}> {
  return requireModule().plan(request);
}

export function startJob(request: ConversionRequest): Promise<string> {
  return requireModule().startJob(request);
}

export function cancelJob(jobId: string): Promise<boolean> {
  return requireModule().cancelJob(jobId);
}

export function pauseJob(jobId: string): Promise<boolean> {
  return requireModule().pauseJob(jobId);
}

export function resumeJob(jobId: string): Promise<boolean> {
  return requireModule().resumeJob(jobId);
}

export function retryJob(jobId: string): Promise<boolean> {
  return requireModule().retryJob(jobId);
}

export function deleteJob(jobId: string): Promise<boolean> {
  return requireModule().deleteJob(jobId);
}

export function updateResources(
  resources: ResourceUpdate,
): Promise<boolean> {
  return requireModule().updateResources(resources);
}

export function getJob(jobId: string): Promise<JobSnapshot | null> {
  return requireModule().getJob(jobId);
}

export function listJobs(): Promise<JobSnapshot[]> {
  return requireModule().listJobs();
}

export function describeBackends(): Promise<string[]> {
  return requireModule().describeBackends();
}

/**
 * Narrows an untyped event-emitter payload to a job event. Unknown shapes
 * are ignored rather than coerced.
 */
export function isJobEventPayload(value: unknown): value is JobEventPayload {
  if (typeof value !== 'object' || value === null) {
    return false;
  }
  const record = value as Record<string, unknown>;
  return (
    typeof record.jobId === 'string' &&
    typeof record.state === 'string' &&
    typeof record.progress === 'number' &&
    (record.outputSizeBytes === undefined ||
      record.outputSizeBytes === null ||
      typeof record.outputSizeBytes === 'number')
  );
}

/**
 * Subscribes to native job events. Registers the listener on the native
 * side (`addListener`, per the TurboModule event contract) and on the JS
 * DeviceEventEmitter. Returns an unsubscribe function.
 *
 * When the bridge is not linked, this is a no-op that returns a no-op
 * unsubscribe — callers surface unavailability through the store instead.
 */
export function subscribeToJobEvents(
  handler: (payload: JobEventPayload) => void,
): () => void {
  const module = getModule();
  if (!module) {
    return () => {};
  }

  module.addListener(JOB_EVENT_NAME);
  const subscription = DeviceEventEmitter.addListener(
    JOB_EVENT_NAME,
    (raw: unknown) => {
      if (isJobEventPayload(raw)) {
        handler(raw);
      }
    },
  );

  return () => {
    subscription.remove();
    module.removeListeners(1);
  };
}

/**
 * Human-readable message for engine failures, with first-class handling of
 * the not-linked case.
 */
export function describeEngineError(error: unknown): string {
  if (error instanceof EngineBridgeUnavailable) {
    return error.message;
  }
  if (error instanceof Error) {
    return error.message || 'Engine request failed';
  }
  return 'Engine request failed';
}
