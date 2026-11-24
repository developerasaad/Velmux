/**
 * Product-controlled option lists (see docs/FEATURE_MATRIX.md).
 *
 * The UI only ever exposes valid combinations. Audio-only containers imply
 * an audio-only output; the deeper codec/container validation belongs to
 * the engine planner, and `plan()` reports infeasible combinations.
 */

export interface ContainerOption {
  id: string;
  label: string;
  kind: 'video' | 'audio';
}

export const CONTAINERS: readonly ContainerOption[] = [
  {id: 'MP4', label: 'MP4', kind: 'video'},
  {id: 'M4V', label: 'M4V', kind: 'video'},
  {id: 'MOV', label: 'MOV', kind: 'video'},
  {id: 'MKV', label: 'MKV', kind: 'video'},
  {id: 'WebM', label: 'WebM', kind: 'video'},
  {id: 'AVI', label: 'AVI', kind: 'video'},
  {id: 'MPEG-TS', label: 'MPEG-TS', kind: 'video'},
  {id: 'FLV', label: 'FLV', kind: 'video'},
  {id: '3GP', label: '3GP', kind: 'video'},
  {id: 'M4A', label: 'M4A', kind: 'audio'},
  {id: 'Ogg', label: 'Ogg', kind: 'audio'},
  {id: 'MP3', label: 'MP3', kind: 'audio'},
  {id: 'WAV', label: 'WAV', kind: 'audio'},
  {id: 'FLAC', label: 'FLAC', kind: 'audio'},
];

export function getContainer(id: string | null): ContainerOption | undefined {
  return CONTAINERS.find(container => container.id === id);
}

export function isAudioOnlyContainer(id: string | null): boolean {
  return getContainer(id)?.kind === 'audio';
}

export interface ResolutionOption {
  id: 'original' | '2160p' | '1440p' | '1080p' | '720p' | '480p' | '360p' | 'custom';
  label: string;
  height: number | null;
}

export const RESOLUTIONS: readonly ResolutionOption[] = [
  {id: 'original', label: 'Original', height: null},
  {id: '2160p', label: '2160p', height: 2160},
  {id: '1440p', label: '1440p', height: 1440},
  {id: '1080p', label: '1080p', height: 1080},
  {id: '720p', label: '720p', height: 720},
  {id: '480p', label: '480p', height: 480},
  {id: '360p', label: '360p', height: 360},
  {id: 'custom', label: 'Custom', height: null},
];

export interface FrameRateOption {
  id: string;
  label: string;
  value: number | null;
}

export const FRAME_RATES: readonly FrameRateOption[] = [
  {id: 'original', label: 'Original', value: null},
  {id: '60', label: '60', value: 60},
  {id: '30', label: '30', value: 30},
  {id: '24', label: '24', value: 24},
];

/** Video encoders exposed under Advanced (feature registry, encode only). */
export const VIDEO_ENCODERS: readonly string[] = [
  'H.264',
  'H.265',
  'AV1',
  'VP9',
  'VP8',
  'MPEG-4',
  'FFV1',
  'MJPEG',
];

/** Audio encoders exposed under Audio and Advanced (encode only). */
export const AUDIO_ENCODERS: readonly string[] = [
  'AAC-LC',
  'HE-AAC',
  'MP3',
  'Opus',
  'Vorbis',
  'FLAC',
  'ALAC',
  'PCM',
  'AC-3',
  'E-AC-3',
  'WavPack',
  'AMR-NB',
  'AMR-WB',
];

export const AUDIO_BITRATES_KBPS: readonly number[] = [
  96, 128, 160, 192, 256, 320,
];

export interface SampleRateOption {
  label: string;
  value: number | null;
}

export const SAMPLE_RATES: readonly SampleRateOption[] = [
  {label: 'Original', value: null},
  {label: '48 kHz', value: 48000},
  {label: '44.1 kHz', value: 44100},
  {label: '96 kHz', value: 96000},
];

export interface ChannelOption {
  label: string;
  value: number | null;
}

export const CHANNEL_OPTIONS: readonly ChannelOption[] = [
  {label: 'Original', value: null},
  {label: 'Mono', value: 1},
  {label: 'Stereo', value: 2},
  {label: '5.1', value: 6},
];

export const VIDEO_PROFILES: readonly string[] = [
  'Auto',
  'Baseline',
  'Main',
  'High',
];

export const VIDEO_LEVELS: readonly string[] = [
  'Auto',
  '4.0',
  '4.2',
  '5.0',
  '5.1',
  '5.2',
];

// Engine-supported encode pixel formats (request contract): 8/10-bit
// planar YUV in 4:2:0/4:2:2/4:4:4. Capture-oriented formats (nv12,
// p010le) and 12-bit are not encode targets and stay off the list.
export const PIXEL_FORMATS: readonly string[] = [
  'Auto',
  'yuv420p',
  'yuv422p',
  'yuv444p',
];

export const BIT_DEPTHS: readonly number[] = [8, 10];

// Geometry and HDR option lists. The literal unions mirror the request
// contract (`ConversionRequest`: quarter-turn `rotationDegrees`, `hdrPolicy`).

export interface RotationOption {
  label: string;
  value: 0 | 90 | 180 | 270;
}

export const ROTATIONS: readonly RotationOption[] = [
  {label: 'Original', value: 0},
  {label: '90°', value: 90},
  {label: '180°', value: 180},
  {label: '270°', value: 270},
];

export interface HdrPolicyOption {
  label: string;
  value: 'auto' | 'toneMapToSdr';
}

/** HDR handling: auto preserves HDR where the output can carry it. */
export const HDR_POLICIES: readonly HdrPolicyOption[] = [
  {label: 'Auto', value: 'auto'},
  {label: 'Tone map to SDR', value: 'toneMapToSdr'},
];
