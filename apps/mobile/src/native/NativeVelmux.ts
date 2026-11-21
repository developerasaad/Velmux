import type {TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

export interface MediaSource {
  uri: string;
  displayName: string;
}
export interface VideoStreamInfo {
  codec: string;
  width: number;
  height: number;
  frameRate: number;
  bitRate: number;
  pixelFormat: string;
  hdr: boolean;
}
export interface AudioStreamInfo {
  codec: string;
  channels: number;
  sampleRate: number;
  bitRate: number;
  language: string | null;
}
export interface SubtitleStreamInfo {
  format: string;
  language: string | null;
  isDefault: boolean;
  isForced: boolean;
}
export interface MediaInfo {
  durationSec: number;
  container: string;
  sizeBytes: number;
  video: VideoStreamInfo[];
  audio: AudioStreamInfo[];
  subtitles: SubtitleStreamInfo[];
}
export interface CompressionGoal {
  kind: 'recommended' | 'smallerFile' | 'highQuality' | 'fitUnderSize' | 'custom';
  targetSizeBytes?: number;
}
export interface ConversionRequest {
  source: MediaSource;
  outputName: string | null;
  outputContainer: string | null;
  goal: CompressionGoal;
  video: {
    enabled: boolean;
    codec: string | null;
    resolutionHeight: number | null;
    frameRate: number | null;
    rateControl: 'auto' | 'bitrate' | 'quality';
    bitRate: number | null;
    quality: number | null;
  };
  audio: {
    enabled: boolean;
    codec: string | null;
    bitRate: number | null;
    sampleRate: number | null;
    channels: number | null;
  };
  subtitles: {
    policy: 'preserve' | 'remove' | 'extract' | 'burnIn';
    externalUri: string | null;
    timingOffsetMs: number;
  };
  processingMode: 'auto' | 'preferNative' | 'preferSoftware' | 'balanced';
}
export interface JobSnapshot {
  id: string;
  state:
    | 'queued'
    | 'preparing'
    | 'running'
    | 'checkpointing'
    | 'paused'
    | 'recovering'
    | 'interrupted'
    | 'failed'
    | 'canceled'
    | 'validating'
    | 'completed';
  progress: number; // 0..1, or -1 when unknown
  sourceName: string;
  outputName: string | null;
  error: string | null;
  outputSizeBytes: number | null;
  inputSizeBytes: number | null;
  pipelineSummary: string | null;
}
export interface Spec extends TurboModule {
  inspect(source: MediaSource): Promise<MediaInfo>;
  plan(request: ConversionRequest): Promise<{
    estimatedOutputBytes: number | null;
    pipelineSummary: string;
    feasible: boolean;
    message: string | null;
  }>;
  startJob(request: ConversionRequest): Promise<string>;
  cancelJob(jobId: string): Promise<boolean>;
  pauseJob(jobId: string): Promise<boolean>;
  resumeJob(jobId: string): Promise<boolean>;
  getJob(jobId: string): Promise<JobSnapshot | null>;
  listJobs(): Promise<JobSnapshot[]>;
  describeBackends(): Promise<string[]>;
  addListener(eventName: string): Promise<void>;
  removeListeners(count: number): Promise<void>;
}
export default TurboModuleRegistry.getEnforcing<Spec>('VelmuxEngine');
