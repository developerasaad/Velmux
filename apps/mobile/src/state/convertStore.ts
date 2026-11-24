import {create} from 'zustand';

import {
  isAudioOnlyContainer,
  RESOLUTIONS,
} from '../constants/options';
import type {ResolutionOption} from '../constants/options';
import type {
  CompressionGoal,
  ConversionRequest,
  MediaSource,
} from '../native/NativeVelmux';
import {parsePositiveInt, parseTargetSizeBytes} from '../format/format';
import {useSettingsStore} from './settingsStore';
import type {ProcessingPreference} from './settingsStore';

/**
 * The conversion workspace draft. Field values live here so that closing
 * the Advanced section (or navigating away and back) never discards
 * changes.
 *
 * Preset semantics (see docs/PRODUCT.md):
 * - The goal selector expresses intent; the engine derives parameters.
 * - Fields exposed inside the Advanced section are "advanced fields".
 *   Modifying any of them while a preset is active relabels the goal as
 *   "Custom — based on <preset>".
 * - Switching to a different preset re-derives the engine-side parameters,
 *   so technical overrides from the previous preset do not carry over.
 *
 * Edit controls (trim, crop, rotation, flips, HDR policy, audio DSP,
 * audio track selection, replace audio, cover art) describe what happens
 * to the media rather than how the engine reaches the goal, so they are
 * preset-independent: changing them never relabels the goal and they
 * survive preset switches and "Reset to preset defaults".
 *
 * Replace audio and audio track selection are mutually exclusive (the
 * engine planner rejects replace + multi-track). The setters keep the
 * draft exclusive and `computeRequest` enforces it defensively.
 */

export type GoalKind = CompressionGoal['kind'];
export type SubtitlePolicy = ConversionRequest['subtitles']['policy'];
export type RateControl = ConversionRequest['video']['rateControl'];
export type RotationDegrees = ConversionRequest['video']['rotationDegrees'];
export type HdrPolicy = ConversionRequest['video']['hdrPolicy'];
export type TargetSizeUnit = 'MB' | 'GB';
export type MetadataBehavior = 'preserve' | 'strip';

/** Window on the source timeline in seconds; endSec null = open-ended. */
export interface TrimSelection {
  startSec: number;
  endSec: number | null;
}

/** Crop rectangle over the source frame, in source pixels. */
export interface CropSelection {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** How the source's audio tracks are carried into the output. */
export type AudioTrackMode = 'first' | 'all' | 'select';

/** Patch accepted by `setAudioDsp`; omitted keys keep their draft values. */
export interface AudioDspPatch {
  gainDb?: number | null;
  fadeInSec?: number | null;
  fadeOutSec?: number | null;
  playbackSpeed?: number | null;
  normalizeLoudness?: boolean;
  removeSilence?: boolean;
}

export interface AdvancedExtras {
  profile: string | null;
  level: string | null;
  pixelFormat: string | null;
  bitDepth: number | null;
  gopSize: number | null;
  bFrames: number | null;
  metadata: MetadataBehavior;
}

export interface ConvertDraft {
  source: MediaSource | null;
  basePreset: GoalKind;
  targetSizeText: string;
  targetSizeUnit: TargetSizeUnit;
  outputContainer: string | null;
  resolutionChoice: ResolutionOption['id'];
  customResolutionText: string;
  /** Timeline window; null = convert the whole source. */
  trim: TrimSelection | null;
  /** Crop rectangle; null = keep the full frame. */
  crop: CropSelection | null;
  /** Quarter-turn rotation applied after cropping (engine order). */
  rotationDegrees: RotationDegrees;
  flipHorizontal: boolean;
  flipVertical: boolean;
  video: {
    enabled: boolean;
    codec: string | null;
    frameRate: number | null;
    rateControl: RateControl;
    bitRate: number | null;
    quality: number | null;
    hdrPolicy: HdrPolicy;
  };
  audio: {
    enabled: boolean;
    codec: string | null;
    bitRate: number | null;
    sampleRate: number | null;
    channels: number | null;
    // Audio DSP chain. Durations live in seconds here; the request
    // contract carries them as milliseconds.
    gainDb: number | null;
    fadeInSec: number | null;
    fadeOutSec: number | null;
    playbackSpeed: number | null;
    normalizeLoudness: boolean;
    removeSilence: boolean;
    // Multi-track: -1 = first track; a valid index selects that track.
    sourceTrackIndex: number;
    allTracks: boolean;
    replaceAudioUri: string | null;
    // Cover art (the request contract carries it under `audio`).
    coverArtUri: string | null;
    preserveCoverArt: boolean;
  };
  subtitles: {
    policy: SubtitlePolicy;
    externalUri: string | null;
    externalName: string | null;
    timingOffsetMs: number;
  };
  processingMode: ProcessingPreference;
  extras: AdvancedExtras;
  advancedDirty: boolean;
}

export const PRESET_LABELS: Record<GoalKind, string> = {
  recommended: 'Recommended',
  smallerFile: 'Smaller File',
  fitUnderSize: 'Fit Under Size',
  highQuality: 'High Quality',
  custom: 'Custom',
};

export interface BeginDraftOptions {
  /** Entry intent from Home, or 'reuse' when reusing a previous request. */
  intent?: 'compress' | 'convert' | 'audioExtract' | 'reuse';
  request?: ConversionRequest;
}

export interface ConvertState {
  draft: ConvertDraft;
  beginDraft: (source: MediaSource, options?: BeginDraftOptions) => void;
  setBasePreset: (preset: GoalKind) => void;
  setTargetSizeText: (text: string) => void;
  setTargetSizeUnit: (unit: TargetSizeUnit) => void;
  setOutputContainer: (container: string | null) => void;
  setResolutionChoice: (choice: ResolutionOption['id']) => void;
  setCustomResolutionText: (text: string) => void;
  setVideoEnabled: (enabled: boolean) => void;
  setAudioEnabled: (enabled: boolean) => void;
  setExternalSubtitle: (
    subtitle: {uri: string; name: string} | null,
  ) => void;
  // Advanced-surface setters (these mark the preset as modified).
  setVideoCodec: (codec: string | null) => void;
  setFrameRate: (frameRate: number | null) => void;
  setRateControl: (rateControl: RateControl) => void;
  setVideoBitRate: (bitRate: number | null) => void;
  setVideoQuality: (quality: number | null) => void;
  setAudioCodec: (codec: string | null) => void;
  setAudioBitRate: (bitRate: number | null) => void;
  setAudioSampleRate: (sampleRate: number | null) => void;
  setAudioChannels: (channels: number | null) => void;
  setSubtitlePolicy: (policy: SubtitlePolicy) => void;
  setTimingOffsetMs: (offsetMs: number) => void;
  setProcessingMode: (mode: ProcessingPreference) => void;
  setExtras: (extras: Partial<AdvancedExtras>) => void;
  // Edit controls (preset-independent; see the file-level docs).
  setTrim: (trim: TrimSelection | null) => void;
  setCrop: (crop: CropSelection | null) => void;
  setRotation: (degrees: RotationDegrees) => void;
  setFlip: (axis: 'horizontal' | 'vertical', enabled: boolean) => void;
  setHdrPolicy: (policy: HdrPolicy) => void;
  /**
   * Patches the audio DSP chain. Values are sanitized: playback speed
   * must be > 0 and fades >= 0 — anything else (including an explicit
   * null/undefined) clears that DSP step back to "no change".
   */
  setAudioDsp: (patch: AudioDspPatch) => void;
  /**
   * Selects which source audio tracks are used. `select` requires a
   * non-negative integer `trackIndex`; anything else falls back to the
   * first track. Choosing a mode clears a pending replace-audio file
   * (track selection means the source audio is used).
   */
  setAudioTrackSelection: (mode: AudioTrackMode, trackIndex?: number) => void;
  /**
   * Replaces the source audio with an external file, or null restores
   * the source audio. Setting a file resets track selection to the
   * first track (the planner rejects replace + multi-track).
   */
  setReplaceAudioUri: (uri: string | null) => void;
  /**
   * Sets external cover art and whether embedded cover art is preserved.
   * `preserve` defaults to the current draft value.
   */
  setCoverArt: (uri: string | null, preserve?: boolean) => void;
  resetAdvanced: () => void;
  // Derived helpers.
  toRequest: () => ConversionRequest | null;
  effectiveGoal: () => CompressionGoal;
  goalLabel: () => string;
  targetSizeBytes: () => number | null;
  resolutionHeight: () => number | null;
}

function defaultExtras(): AdvancedExtras {
  return {
    profile: null,
    level: null,
    pixelFormat: null,
    bitDepth: null,
    gopSize: null,
    bFrames: null,
    metadata: 'preserve',
  };
}

function autoDerivedVideo(): ConvertDraft['video'] {
  return {
    enabled: true,
    codec: null,
    frameRate: null,
    rateControl: 'auto',
    bitRate: null,
    quality: null,
    hdrPolicy: 'auto',
  };
}

function defaultAudioDraft(): ConvertDraft['audio'] {
  return {
    enabled: true,
    codec: null,
    bitRate: null,
    sampleRate: null,
    channels: null,
    gainDb: null,
    fadeInSec: null,
    fadeOutSec: null,
    playbackSpeed: null,
    normalizeLoudness: false,
    removeSilence: false,
    sourceTrackIndex: -1,
    allTracks: false,
    replaceAudioUri: null,
    coverArtUri: null,
    preserveCoverArt: true,
  };
}

/**
 * Normalizes a trim window: a missing/non-finite start clamps to 0, a
 * missing end stays open-ended, and an end before the start clamps to
 * the start — the emitted request never describes a negative window.
 */
function sanitizeTrim(trim: TrimSelection | null): TrimSelection | null {
  if (!trim) {
    return null;
  }
  const startSec = Number.isFinite(trim.startSec)
    ? Math.max(0, trim.startSec)
    : 0;
  const endSec =
    trim.endSec != null && Number.isFinite(trim.endSec)
      ? Math.max(startSec, trim.endSec)
      : null;
  return {startSec, endSec};
}

/**
 * Normalizes a crop rectangle: coordinates clamp to >= 0, and a rect
 * without positive finite dimensions is dropped (null = no crop).
 */
function sanitizeCrop(crop: CropSelection | null): CropSelection | null {
  if (!crop) {
    return null;
  }
  const {x, y, width, height} = crop;
  if (
    !Number.isFinite(x) ||
    !Number.isFinite(y) ||
    !Number.isFinite(width) ||
    !Number.isFinite(height) ||
    width <= 0 ||
    height <= 0
  ) {
    return null;
  }
  return {x: Math.max(0, x), y: Math.max(0, y), width, height};
}

/** Gain is unconstrained (negative gain attenuates); non-finite clears. */
function sanitizeGainDb(gainDb: number | null): number | null {
  return gainDb != null && Number.isFinite(gainDb) ? gainDb : null;
}

/** Fades must be non-negative; anything else clears the fade. */
function sanitizeFadeSec(fadeSec: number | null): number | null {
  return fadeSec != null && Number.isFinite(fadeSec) && fadeSec >= 0
    ? fadeSec
    : null;
}

/** Speed must be positive; anything else falls back to the original. */
function sanitizeSpeed(speed: number | null): number | null {
  return speed != null && Number.isFinite(speed) && speed > 0
    ? speed
    : null;
}

/** Seconds -> milliseconds, rounding to the nearest millisecond. */
function secToMs(sec: number | null): number | null {
  return sec == null ? null : Math.round(sec * 1000);
}

/** Milliseconds -> seconds. */
function msToSec(ms: number | null): number | null {
  return ms == null ? null : ms / 1000;
}

function freshDraft(source: MediaSource, intent: BeginDraftOptions['intent']): ConvertDraft {
  const settings = useSettingsStore.getState();
  const draft: ConvertDraft = {
    source,
    // "Compress" expresses an explicit size-first intent; other entries
    // use the user's default goal.
    basePreset: intent === 'compress' ? 'smallerFile' : settings.defaultGoal,
    targetSizeText: '',
    targetSizeUnit: 'MB',
    outputContainer: settings.defaultOutputContainer,
    resolutionChoice: 'original',
    customResolutionText: '',
    trim: null,
    crop: null,
    rotationDegrees: 0,
    flipHorizontal: false,
    flipVertical: false,
    video: autoDerivedVideo(),
    audio: defaultAudioDraft(),
    subtitles: {
      policy: 'preserve',
      externalUri: null,
      externalName: null,
      timingOffsetMs: 0,
    },
    processingMode: settings.processingPreference,
    extras: defaultExtras(),
    advancedDirty: false,
  };

  if (intent === 'audioExtract') {
    // Audio extraction: audio-only output container, no video track.
    // Subtitles are extracted rather than silently dropped (product rule).
    draft.outputContainer = 'M4A';
    draft.video.enabled = false;
    draft.subtitles.policy = 'extract';
  }
  return draft;
}

function draftFromRequest(request: ConversionRequest): ConvertDraft {
  const goal = request.goal;
  const targetBytes = goal.targetSizeBytes ?? null;
  const useGB = targetBytes !== null && targetBytes >= 1_000_000_000;
  const divisor = useGB ? 1_000_000_000 : 1_000_000;
  const targetText =
    targetBytes === null
      ? ''
      : String(
          Math.round((targetBytes / divisor) * 10) / 10,
        ).replace(/\.0$/, '');

  const resolution = RESOLUTIONS.find(
    option => option.height === request.video.resolutionHeight,
  );

  const audio = request.audio;
  // Replace audio wins over track selection (planner rule); keep the
  // imported draft as exclusive as the setters would have left it.
  const replacingAudio = audio.replaceAudioUri != null;

  return {
    source: request.source,
    basePreset: goal.kind,
    targetSizeText: targetText,
    targetSizeUnit: useGB ? 'GB' : 'MB',
    outputContainer: request.outputContainer,
    resolutionChoice: resolution ? resolution.id : 'custom',
    customResolutionText:
      request.video.resolutionHeight !== null && !resolution
        ? String(request.video.resolutionHeight)
        : '',
    trim: sanitizeTrim(
      request.trim
        ? {
            startSec: request.trim.startUs / 1e6,
            endSec:
              request.trim.endUs == null
                ? null
                : request.trim.endUs / 1e6,
          }
        : null,
    ),
    crop: sanitizeCrop(request.video.crop),
    rotationDegrees: request.video.rotationDegrees,
    flipHorizontal: request.video.flipHorizontal,
    flipVertical: request.video.flipVertical,
    video: {
      enabled: request.video.enabled,
      codec: request.video.codec,
      frameRate: request.video.frameRate,
      rateControl: request.video.rateControl,
      bitRate: request.video.bitRate,
      quality: request.video.quality,
      hdrPolicy: request.video.hdrPolicy,
    },
    audio: {
      enabled: audio.enabled,
      codec: audio.codec,
      bitRate: audio.bitRate,
      sampleRate: audio.sampleRate,
      channels: audio.channels,
      gainDb: sanitizeGainDb(audio.gainDb),
      fadeInSec: sanitizeFadeSec(msToSec(audio.fadeInMs)),
      fadeOutSec: sanitizeFadeSec(msToSec(audio.fadeOutMs)),
      playbackSpeed: sanitizeSpeed(audio.playbackSpeed),
      normalizeLoudness: audio.normalizeLoudness,
      removeSilence: audio.removeSilence,
      sourceTrackIndex: replacingAudio ? -1 : audio.sourceTrackIndex,
      allTracks: replacingAudio ? false : audio.allTracks,
      replaceAudioUri: audio.replaceAudioUri,
      coverArtUri: audio.coverArtUri,
      preserveCoverArt: audio.preserveCoverArt,
    },
    subtitles: {
      policy: request.subtitles.policy,
      externalUri: request.subtitles.externalUri,
      externalName: null,
      timingOffsetMs: request.subtitles.timingOffsetMs,
    },
    processingMode: request.processingMode,
    extras: defaultExtras(),
    advancedDirty: false,
  };
}

/** Mutates a draft so an audio-only container keeps the request coherent. */
function applyContainerConstraint(
  draft: ConvertDraft,
  container: string | null,
): ConvertDraft {
  if (isAudioOnlyContainer(container)) {
    return {...draft, outputContainer: container, video: {...draft.video, enabled: false}};
  }
  return {...draft, outputContainer: container};
}

export const useConvertStore = create<ConvertState>()((set, get) => ({
  draft: {
    source: null,
    basePreset: 'recommended',
    targetSizeText: '',
    targetSizeUnit: 'MB',
    outputContainer: null,
    resolutionChoice: 'original',
    customResolutionText: '',
    trim: null,
    crop: null,
    rotationDegrees: 0,
    flipHorizontal: false,
    flipVertical: false,
    video: autoDerivedVideo(),
    audio: defaultAudioDraft(),
    subtitles: {
      policy: 'preserve',
      externalUri: null,
      externalName: null,
      timingOffsetMs: 0,
    },
    processingMode: 'auto',
    extras: defaultExtras(),
    advancedDirty: false,
  },

  beginDraft: (source, options) => {
    if (options?.intent === 'reuse' && options.request) {
      set({draft: draftFromRequest(options.request)});
      return;
    }
    set({draft: freshDraft(source, options?.intent)});
  },

  setBasePreset: preset => {
    set(state => ({
      draft: {
        ...state.draft,
        basePreset: preset,
        // A new preset re-derives engine-side parameters.
        video: {...autoDerivedVideo(), enabled: state.draft.video.enabled},
        extras: defaultExtras(),
        advancedDirty: false,
      },
    }));
  },

  setTargetSizeText: text =>
    set(state => ({draft: {...state.draft, targetSizeText: text}})),

  setTargetSizeUnit: unit =>
    set(state => ({draft: {...state.draft, targetSizeUnit: unit}})),

  setOutputContainer: container =>
    set(state => ({
      draft: applyContainerConstraint(state.draft, container),
    })),

  setResolutionChoice: choice =>
    set(state => ({
      draft: {...state.draft, resolutionChoice: choice},
    })),

  setCustomResolutionText: text =>
    set(state => ({
      draft: {...state.draft, customResolutionText: text},
    })),

  setVideoEnabled: enabled => {
    set(state => {
      // A video track cannot be enabled into an audio-only container; the
      // output format selector owns that transition.
      if (enabled && isAudioOnlyContainer(state.draft.outputContainer)) {
        return state;
      }
      return {draft: {...state.draft, video: {...state.draft.video, enabled}}};
    });
  },

  setAudioEnabled: enabled =>
    set(state => ({
      draft: {...state.draft, audio: {...state.draft.audio, enabled}},
    })),

  setExternalSubtitle: subtitle =>
    set(state => ({
      draft: {
        ...state.draft,
        subtitles: {
          ...state.draft.subtitles,
          externalUri: subtitle ? subtitle.uri : null,
          externalName: subtitle ? subtitle.name : null,
        },
      },
    })),

  setVideoCodec: codec =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        video: {...state.draft.video, codec},
      },
    })),

  setFrameRate: frameRate =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        video: {...state.draft.video, frameRate},
      },
    })),

  setRateControl: rateControl =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        video: {...state.draft.video, rateControl},
      },
    })),

  setVideoBitRate: bitRate =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        video: {...state.draft.video, bitRate},
      },
    })),

  setVideoQuality: quality =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        video: {...state.draft.video, quality},
      },
    })),

  setAudioCodec: codec =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        audio: {...state.draft.audio, codec},
      },
    })),

  setAudioBitRate: bitRate =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        audio: {...state.draft.audio, bitRate},
      },
    })),

  setAudioSampleRate: sampleRate =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        audio: {...state.draft.audio, sampleRate},
      },
    })),

  setAudioChannels: channels =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        audio: {...state.draft.audio, channels},
      },
    })),

  setSubtitlePolicy: policy =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        subtitles: {...state.draft.subtitles, policy},
      },
    })),

  setTimingOffsetMs: timingOffsetMs =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        subtitles: {...state.draft.subtitles, timingOffsetMs},
      },
    })),

  setProcessingMode: processingMode =>
    set(state => ({
      draft: {...state.draft, processingMode, advancedDirty: true},
    })),

  setExtras: extrasPatch =>
    set(state => ({
      draft: {
        ...state.draft,
        advancedDirty: true,
        extras: {...state.draft.extras, ...extrasPatch},
      },
    })),

  setTrim: trim =>
    set(state => ({
      draft: {...state.draft, trim: sanitizeTrim(trim)},
    })),

  setCrop: crop =>
    set(state => ({
      draft: {...state.draft, crop: sanitizeCrop(crop)},
    })),

  setRotation: rotationDegrees =>
    set(state => ({
      draft: {...state.draft, rotationDegrees},
    })),

  setFlip: (axis, enabled) =>
    set(state => ({
      draft: {
        ...state.draft,
        flipHorizontal:
          axis === 'horizontal' ? enabled : state.draft.flipHorizontal,
        flipVertical: axis === 'vertical' ? enabled : state.draft.flipVertical,
      },
    })),

  setHdrPolicy: hdrPolicy =>
    set(state => ({
      draft: {
        ...state.draft,
        video: {...state.draft.video, hdrPolicy},
      },
    })),

  setAudioDsp: patch =>
    set(state => {
      const current = state.draft.audio;
      const next: ConvertDraft['audio'] = {...current};
      if ('gainDb' in patch) {
        next.gainDb = sanitizeGainDb(patch.gainDb ?? null);
      }
      if ('fadeInSec' in patch) {
        next.fadeInSec = sanitizeFadeSec(patch.fadeInSec ?? null);
      }
      if ('fadeOutSec' in patch) {
        next.fadeOutSec = sanitizeFadeSec(patch.fadeOutSec ?? null);
      }
      if ('playbackSpeed' in patch) {
        next.playbackSpeed = sanitizeSpeed(patch.playbackSpeed ?? null);
      }
      if (patch.normalizeLoudness !== undefined) {
        next.normalizeLoudness = patch.normalizeLoudness;
      }
      if (patch.removeSilence !== undefined) {
        next.removeSilence = patch.removeSilence;
      }
      return {draft: {...state.draft, audio: next}};
    }),

  setAudioTrackSelection: (mode, trackIndex) =>
    set(state => {
      const audio: ConvertDraft['audio'] = {...state.draft.audio};
      if (mode === 'all') {
        audio.allTracks = true;
        audio.sourceTrackIndex = -1;
      } else if (mode === 'select') {
        const index = trackIndex ?? audio.sourceTrackIndex;
        audio.allTracks = false;
        audio.sourceTrackIndex =
          index != null && Number.isInteger(index) && index >= 0
            ? index
            : -1;
      } else {
        audio.allTracks = false;
        audio.sourceTrackIndex = -1;
      }
      // Track selection means the source audio is used, so a pending
      // replacement (mutually exclusive at the planner) is dropped.
      audio.replaceAudioUri = null;
      return {draft: {...state.draft, audio}};
    }),

  setReplaceAudioUri: uri =>
    set(state => {
      const replaceAudioUri = uri != null && uri.length > 0 ? uri : null;
      const audio: ConvertDraft['audio'] = {
        ...state.draft.audio,
        replaceAudioUri,
      };
      if (replaceAudioUri != null) {
        // Replace audio and multi-track selection are mutually exclusive
        // at the planner; choosing a replacement resets to first track.
        audio.allTracks = false;
        audio.sourceTrackIndex = -1;
      }
      return {draft: {...state.draft, audio}};
    }),

  setCoverArt: (uri, preserve) =>
    set(state => ({
      draft: {
        ...state.draft,
        audio: {
          ...state.draft.audio,
          coverArtUri: uri != null && uri.length > 0 ? uri : null,
          preserveCoverArt: preserve ?? state.draft.audio.preserveCoverArt,
        },
      },
    })),

  resetAdvanced: () =>
    set(state => ({
      draft: {
        ...state.draft,
        video: {
          ...autoDerivedVideo(),
          enabled: state.draft.video.enabled,
        },
        extras: defaultExtras(),
        processingMode: useSettingsStore.getState().processingPreference,
        advancedDirty: false,
      },
    })),

  targetSizeBytes: () => computeTargetSizeBytes(get().draft),

  resolutionHeight: () => computeResolutionHeight(get().draft),

  effectiveGoal: () => computeEffectiveGoal(get().draft),

  goalLabel: () => computeGoalLabel(get().draft),

  toRequest: () => computeRequest(get().draft),
}));

/** Serializes the request for change detection (estimate re-planning). */
export function requestSignature(request: ConversionRequest): string {
  return JSON.stringify(request);
}

/** Pure helpers usable as render-time selectors over the draft. */

export function computeTargetSizeBytes(draft: ConvertDraft): number | null {
  return parseTargetSizeBytes(draft.targetSizeText, draft.targetSizeUnit);
}

export function computeResolutionHeight(draft: ConvertDraft): number | null {
  if (draft.resolutionChoice === 'original') {
    return null;
  }
  if (draft.resolutionChoice === 'custom') {
    return parsePositiveInt(draft.customResolutionText);
  }
  const option = RESOLUTIONS.find(
    candidate => candidate.id === draft.resolutionChoice,
  );
  return option?.height ?? null;
}

/** Renders the draft's track selection back into a UI mode. */
export function computeAudioTrackMode(draft: ConvertDraft): AudioTrackMode {
  if (draft.audio.allTracks) {
    return 'all';
  }
  if (draft.audio.sourceTrackIndex >= 0) {
    return 'select';
  }
  return 'first';
}

export function computeEffectiveGoal(draft: ConvertDraft): CompressionGoal {
  if (draft.basePreset === 'fitUnderSize') {
    // The size constraint is an explicit user constraint; it survives
    // advanced overrides (which refine within the budget).
    const target = computeTargetSizeBytes(draft);
    return target === null
      ? {kind: 'fitUnderSize'}
      : {kind: 'fitUnderSize', targetSizeBytes: target};
  }
  if (draft.basePreset === 'custom' || draft.advancedDirty) {
    return {kind: 'custom'};
  }
  return {kind: draft.basePreset};
}

export function computeGoalLabel(draft: ConvertDraft): string {
  if (draft.basePreset === 'custom') {
    return PRESET_LABELS.custom;
  }
  if (draft.advancedDirty) {
    return `Custom — based on ${PRESET_LABELS[draft.basePreset]}`;
  }
  return PRESET_LABELS[draft.basePreset];
}

/**
 * Maps the draft's advanced extras (profile, level, chroma, bit depth)
 * onto the engine's pixel-format contract. Returns null when nothing was
 * selected (encoder default). Unsupported combinations cannot occur: the
 * option lists only offer the engine-supported set.
 */
function enginePixelFormat(
  extras: AdvancedExtras,
): ConversionRequest['video']['pixelFormat'] {
  if (!extras.pixelFormat && extras.bitDepth == null) {
    return null;
  }
  const chroma = extras.pixelFormat ?? 'yuv420p';
  const tenBit = extras.bitDepth === 10;
  switch (chroma) {
    case 'yuv422p':
      return tenBit ? 'yuv422p10le' : 'yuv422p';
    case 'yuv444p':
      return tenBit ? 'yuv444p10le' : 'yuv444p';
    default:
      return tenBit ? 'yuv420p10le' : 'yuv420p';
  }
}

export function computeRequest(draft: ConvertDraft): ConversionRequest | null {
  if (!draft.source) {
    return null;
  }
  const profile =
    draft.extras.profile != null
      ? draft.extras.profile.toLowerCase()
      : null;
  const trim = sanitizeTrim(draft.trim);
  // Replace audio makes track selection meaningless (the planner rejects
  // the combination). The setters keep the draft exclusive; the emission
  // enforces it for drafts built any other way.
  const replacingAudio = draft.audio.replaceAudioUri != null;
  return {
    source: draft.source,
    outputName: null,
    outputContainer: draft.outputContainer,
    goal: computeEffectiveGoal(draft),
    video: {
      enabled: draft.video.enabled,
      codec: draft.video.codec,
      resolutionHeight: computeResolutionHeight(draft),
      frameRate: draft.video.frameRate,
      rateControl: draft.video.rateControl,
      bitRate: draft.video.bitRate,
      quality: draft.video.quality,
      // Geometry transforms (engine order: crop -> rotate -> flip).
      crop: sanitizeCrop(draft.crop),
      rotationDegrees: draft.rotationDegrees,
      flipHorizontal: draft.flipHorizontal,
      flipVertical: draft.flipVertical,
      profile,
      level: draft.extras.level,
      pixelFormat: enginePixelFormat(draft.extras),
      gopSize: draft.extras.gopSize,
      bFrames: draft.extras.bFrames,
      hdrPolicy: draft.video.hdrPolicy,
    },
    audio: {
      enabled: draft.audio.enabled,
      codec: draft.audio.codec,
      bitRate: draft.audio.bitRate,
      sampleRate: draft.audio.sampleRate,
      channels: draft.audio.channels,
      // Audio DSP chain (durations in seconds here, milliseconds there).
      gainDb: sanitizeGainDb(draft.audio.gainDb),
      fadeInMs: secToMs(sanitizeFadeSec(draft.audio.fadeInSec)),
      fadeOutMs: secToMs(sanitizeFadeSec(draft.audio.fadeOutSec)),
      normalizeLoudness: draft.audio.normalizeLoudness,
      playbackSpeed: sanitizeSpeed(draft.audio.playbackSpeed),
      removeSilence: draft.audio.removeSilence,
      pcmBitDepth: null,
      sourceTrackIndex: replacingAudio
        ? -1
        : draft.audio.sourceTrackIndex,
      allTracks: replacingAudio ? false : draft.audio.allTracks,
      replaceAudioUri: draft.audio.replaceAudioUri,
      coverArtUri: draft.audio.coverArtUri,
      preserveCoverArt: draft.audio.preserveCoverArt,
    },
    subtitles: {
      policy: draft.subtitles.policy,
      externalUri: draft.subtitles.externalUri,
      timingOffsetMs: draft.subtitles.timingOffsetMs,
    },
    processingMode: draft.processingMode,
    trim: trim
      ? {
          startUs: Math.round(trim.startSec * 1e6),
          endUs: trim.endSec == null ? null : Math.round(trim.endSec * 1e6),
        }
      : null,
    stripMetadata: draft.extras.metadata === 'strip',
  };
}
