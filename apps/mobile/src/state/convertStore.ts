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
 * NOTE (contract gap, owned by the lead): profile, level, pixel format,
 * bit depth, GOP size, B-frames and metadata behavior are part of the
 * product's Advanced surface but are not part of the `ConversionRequest`
 * bridge contract yet. They are held in this draft (`extras`) and flagged
 * for the engine contract extension; `toRequest()` does not silently
 * invent values for them.
 */

export type GoalKind = CompressionGoal['kind'];
export type SubtitlePolicy = ConversionRequest['subtitles']['policy'];
export type RateControl = ConversionRequest['video']['rateControl'];
export type TargetSizeUnit = 'MB' | 'GB';
export type MetadataBehavior = 'preserve' | 'strip';

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
  video: {
    enabled: boolean;
    codec: string | null;
    frameRate: number | null;
    rateControl: RateControl;
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
  };
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
    video: autoDerivedVideo(),
    audio: {
      enabled: true,
      codec: null,
      bitRate: null,
      sampleRate: null,
      channels: null,
    },
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
    video: {
      enabled: request.video.enabled,
      codec: request.video.codec,
      frameRate: request.video.frameRate,
      rateControl: request.video.rateControl,
      bitRate: request.video.bitRate,
      quality: request.video.quality,
    },
    audio: {...request.audio},
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
    video: autoDerivedVideo(),
    audio: {
      enabled: true,
      codec: null,
      bitRate: null,
      sampleRate: null,
      channels: null,
    },
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

export function computeRequest(draft: ConvertDraft): ConversionRequest | null {
  if (!draft.source) {
    return null;
  }
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
    },
    audio: {
      enabled: draft.audio.enabled,
      codec: draft.audio.codec,
      bitRate: draft.audio.bitRate,
      sampleRate: draft.audio.sampleRate,
      channels: draft.audio.channels,
    },
    subtitles: {
      policy: draft.subtitles.policy,
      externalUri: draft.subtitles.externalUri,
      timingOffsetMs: draft.subtitles.timingOffsetMs,
    },
    processingMode: draft.processingMode,
  };
}
