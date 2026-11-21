import type {JobSnapshot} from '../native/NativeVelmux';

export type JobState = JobSnapshot['state'];

/** Terminal states: the job will not transition further. */
const TERMINAL_STATES: ReadonlySet<JobState> = new Set<JobState>([
  'completed',
  'failed',
  'canceled',
]);

export function isTerminalJob(state: JobState): boolean {
  return TERMINAL_STATES.has(state);
}

/** Active states: the job is expected to make progress. */
export function isActiveJob(state: JobState): boolean {
  return (
    !isTerminalJob(state) &&
    state !== 'interrupted' &&
    state !== 'recovering'
  );
}

/** States in which pausing is offered. */
export function canPauseJob(state: JobState): boolean {
  return (
    state === 'queued' ||
    state === 'preparing' ||
    state === 'running' ||
    state === 'checkpointing' ||
    state === 'validating'
  );
}

/** States in which resuming is offered. */
export function canResumeJob(state: JobState): boolean {
  return state === 'paused' || state === 'interrupted';
}

export type JobStateTone = 'muted' | 'info' | 'accent' | 'warning' | 'success' | 'danger';

export function jobStateTone(state: JobState): JobStateTone {
  switch (state) {
    case 'queued':
      return 'muted';
    case 'preparing':
    case 'recovering':
    case 'validating':
      return 'info';
    case 'running':
    case 'checkpointing':
      return 'accent';
    case 'paused':
    case 'interrupted':
      return 'warning';
    case 'failed':
      return 'danger';
    case 'canceled':
      return 'muted';
    case 'completed':
      return 'success';
  }
}

const STATE_LABELS: Record<JobState, string> = {
  queued: 'Queued',
  preparing: 'Preparing',
  running: 'Running',
  checkpointing: 'Checkpointing',
  paused: 'Paused',
  recovering: 'Recovering',
  interrupted: 'Interrupted',
  failed: 'Failed',
  canceled: 'Canceled',
  validating: 'Validating',
  completed: 'Completed',
};

export function jobStateLabel(state: JobState): string {
  return STATE_LABELS[state];
}

/** All contract states; used to narrow untyped event payloads. */
export const KNOWN_JOB_STATES: ReadonlySet<string> = new Set<string>(
  Object.keys(STATE_LABELS),
);

export function isJobState(value: string): value is JobState {
  return KNOWN_JOB_STATES.has(value);
}
