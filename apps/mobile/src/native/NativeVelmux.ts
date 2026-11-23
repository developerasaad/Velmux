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
  /** CEA-608/708 captions detected in the video bitstream. */
  hasClosedCaptions: boolean;
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
    /** Geometry transforms (spec video ops). Order: crop -> rotate -> flip. */
    crop: {x: number; y: number; width: number; height: number} | null;
    rotationDegrees: 0 | 90 | 180 | 270;
    flipHorizontal: boolean;
    flipVertical: boolean;
    /** Advanced encoder controls (spec advanced controls). */
    profile: string | null;
    level: string | null;
    pixelFormat:
      | 'yuv420p'
      | 'yuv422p'
      | 'yuv444p'
      | 'yuv420p10le'
      | 'yuv422p10le'
      | 'yuv444p10le'
      | null;
    gopSize: number | null;
    bFrames: number | null;
    /** HDR handling: auto preserves where the output can carry it. */
    hdrPolicy: 'auto' | 'toneMapToSdr';
  };
  audio: {
    enabled: boolean;
    codec: string | null;
    bitRate: number | null;
    sampleRate: number | null;
    channels: number | null;
    /** Audio DSP chain (spec audio ops). */
    gainDb: number | null;
    fadeInMs: number | null;
    fadeOutMs: number | null;
    normalizeLoudness: boolean;
    playbackSpeed: number | null;
    removeSilence: boolean;
    /** PCM bit depth for PCM/WAV output. */
    pcmBitDepth: 16 | 24 | 32 | null;
    /** Multi-track: -1 = first; a valid index selects that track. */
    sourceTrackIndex: number;
    allTracks: boolean;
    /** Replace source audio with an external file. */
    replaceAudioUri: string | null;
    /** Cover art: external image embed + attached-picture preservation. */
    coverArtUri: string | null;
    preserveCoverArt: boolean;
  };
  subtitles: {
    policy: 'preserve' | 'remove' | 'extract' | 'burnIn';
    externalUri: string | null;
    timingOffsetMs: number;
  };
  processingMode: 'auto' | 'preferNative' | 'preferSoftware' | 'balanced';
  // Window on the source timeline, microseconds (spec §15/§16 trimming).
  // endUs null = open-ended.
  trim: {startUs: number; endUs: number | null} | null;
  /** Metadata policy: false preserves tags, true strips them. */
  stripMetadata: boolean;
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
export interface ResourceUpdate {
  cpuCores?: number;
  memoryAvailableMb?: number;
  thermalPressure?: number; // 0..1, higher = more constrained
  onBattery?: boolean;
  hardwareCodecAvailable?: boolean;
}
export interface SampleResult {
  path: string;
  startUs: number;
  endUs: number;
  sizeBytes: number;
}
export interface Spec extends TurboModule {
  inspect(source: MediaSource): Promise<MediaInfo>;
  plan(request: ConversionRequest): Promise<{
    estimatedOutputBytes: number | null;
    pipelineSummary: string;
    feasible: boolean;
    message: string | null;
  }>;
  // Sample generation (spec §20): synchronously produce a short preview
  // of the requested settings over the densest section of the source.
  // Runs on the module's background worker; resolve carries the sample.
  generateSample(
    request: ConversionRequest,
    sampleSeconds: number
  ): Promise<SampleResult>;
  startJob(request: ConversionRequest): Promise<string>;
  cancelJob(jobId: string): Promise<boolean>;
  pauseJob(jobId: string): Promise<boolean>;
  resumeJob(jobId: string): Promise<boolean>;
  retryJob(jobId: string): Promise<boolean>;
  deleteJob(jobId: string): Promise<boolean>;
  updateResources(resources: ResourceUpdate): Promise<boolean>;
  getJob(jobId: string): Promise<JobSnapshot | null>;
  // 1 while a worker is executing/finalizing the job, 0 when idle. Durable
  // state may already read canceled/paused while the worker unwinds; UIs
  // use this to tell "cancel recorded" from "cancel completed".
  isJobActive(jobId: string): Promise<boolean>;
  listJobs(): Promise<JobSnapshot[]>;
  describeBackends(): Promise<string[]>;
  addListener(eventName: string): Promise<void>;
  removeListeners(count: number): Promise<void>;
}
export default TurboModuleRegistry.getEnforcing<Spec>('VelmuxEngine');
