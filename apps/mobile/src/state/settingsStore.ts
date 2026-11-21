import {create} from 'zustand';

import type {ConversionRequest} from '../native/NativeVelmux';

export type ThemeMode = 'system' | 'dark' | 'light';
export type ProcessingPreference = ConversionRequest['processingMode'];
export type DefaultGoal = 'recommended' | 'smallerFile' | 'highQuality';

export interface SettingsState {
  themeMode: ThemeMode;
  defaultGoal: DefaultGoal;
  defaultOutputContainer: string;
  processingPreference: ProcessingPreference;
  autoResumeOnLaunch: boolean;
  keepRecoverySegments: boolean;
  notifyProgress: boolean;
  notifyCompletion: boolean;
  notifyFailures: boolean;
  setThemeMode: (mode: ThemeMode) => void;
  setDefaultGoal: (goal: DefaultGoal) => void;
  setDefaultOutputContainer: (container: string) => void;
  setProcessingPreference: (preference: ProcessingPreference) => void;
  setAutoResumeOnLaunch: (value: boolean) => void;
  setKeepRecoverySegments: (value: boolean) => void;
  setNotifyProgress: (value: boolean) => void;
  setNotifyCompletion: (value: boolean) => void;
  setNotifyFailures: (value: boolean) => void;
}

/**
 * User preferences for the Velmux UI.
 *
 * TODO: wire AsyncStorage in the integration milestone. Until then the
 * store persists in memory for the lifetime of the JS session only.
 */
export const useSettingsStore = create<SettingsState>()(set => ({
  themeMode: 'system',
  defaultGoal: 'recommended',
  defaultOutputContainer: 'MP4',
  processingPreference: 'auto',
  autoResumeOnLaunch: true,
  keepRecoverySegments: true,
  notifyProgress: false,
  notifyCompletion: true,
  notifyFailures: true,
  setThemeMode: mode => set({themeMode: mode}),
  setDefaultGoal: goal => set({defaultGoal: goal}),
  setDefaultOutputContainer: container =>
    set({defaultOutputContainer: container}),
  setProcessingPreference: preference =>
    set({processingPreference: preference}),
  setAutoResumeOnLaunch: value => set({autoResumeOnLaunch: value}),
  setKeepRecoverySegments: value => set({keepRecoverySegments: value}),
  setNotifyProgress: value => set({notifyProgress: value}),
  setNotifyCompletion: value => set({notifyCompletion: value}),
  setNotifyFailures: value => set({notifyFailures: value}),
}));
