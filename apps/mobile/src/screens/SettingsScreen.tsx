import React, {useEffect} from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from '../components/AppText';
import {Banner} from '../components/Banner';
import {Button} from '../components/Button';
import {OptionChips} from '../components/OptionChips';
import {Screen} from '../components/Screen';
import {Section} from '../components/Section';
import {ToggleRow} from '../components/ToggleRow';
import {CONTAINERS} from '../constants/options';
import {useJobsStore} from '../state/jobsStore';
import {useSettingsStore} from '../state/settingsStore';
import type {
  DefaultGoal,
  ProcessingPreference,
  ThemeMode,
} from '../state/settingsStore';

const THEME_OPTIONS: ReadonlyArray<{label: string; value: ThemeMode}> = [
  {label: 'System', value: 'system'},
  {label: 'Dark', value: 'dark'},
  {label: 'Light', value: 'light'},
];

const GOAL_OPTIONS: ReadonlyArray<{label: string; value: DefaultGoal}> = [
  {label: 'Recommended', value: 'recommended'},
  {label: 'Smaller File', value: 'smallerFile'},
  {label: 'High Quality', value: 'highQuality'},
];

const PROCESSING_OPTIONS: ReadonlyArray<{
  label: string;
  value: ProcessingPreference;
}> = [
  {label: 'Auto', value: 'auto'},
  {label: 'Prefer native hardware', value: 'preferNative'},
  {label: 'Prefer software', value: 'preferSoftware'},
  {label: 'Balanced', value: 'balanced'},
];

const CONTAINER_OPTIONS = CONTAINERS.map(container => ({
  label: container.label,
  value: container.id,
}));

export function SettingsScreen() {
  const themeMode = useSettingsStore(state => state.themeMode);
  const setThemeMode = useSettingsStore(state => state.setThemeMode);
  const defaultGoal = useSettingsStore(state => state.defaultGoal);
  const setDefaultGoal = useSettingsStore(state => state.setDefaultGoal);
  const defaultOutputContainer = useSettingsStore(
    state => state.defaultOutputContainer,
  );
  const setDefaultOutputContainer = useSettingsStore(
    state => state.setDefaultOutputContainer,
  );
  const processingPreference = useSettingsStore(
    state => state.processingPreference,
  );
  const setProcessingPreference = useSettingsStore(
    state => state.setProcessingPreference,
  );
  const autoResumeOnLaunch = useSettingsStore(
    state => state.autoResumeOnLaunch,
  );
  const setAutoResumeOnLaunch = useSettingsStore(
    state => state.setAutoResumeOnLaunch,
  );
  const keepRecoverySegments = useSettingsStore(
    state => state.keepRecoverySegments,
  );
  const setKeepRecoverySegments = useSettingsStore(
    state => state.setKeepRecoverySegments,
  );
  const notifyProgress = useSettingsStore(state => state.notifyProgress);
  const setNotifyProgress = useSettingsStore(state => state.setNotifyProgress);
  const notifyCompletion = useSettingsStore(
    state => state.notifyCompletion,
  );
  const setNotifyCompletion = useSettingsStore(
    state => state.setNotifyCompletion,
  );
  const notifyFailures = useSettingsStore(state => state.notifyFailures);
  const setNotifyFailures = useSettingsStore(
    state => state.setNotifyFailures,
  );

  const bridgeAvailable = useJobsStore(state => state.bridgeAvailable);
  const engineError = useJobsStore(state => state.engineError);
  const backends = useJobsStore(state => state.backends);
  const backendsError = useJobsStore(state => state.backendsError);
  const discoverBackends = useJobsStore(state => state.discoverBackends);

  useEffect(() => {
    discoverBackends();
  }, [discoverBackends]);

  return (
    <Screen title="Settings" scroll>
      <Section title="Appearance" hint="Dark is the default when the system has no preference.">
        <OptionChips
          options={THEME_OPTIONS}
          selected={themeMode}
          onSelect={setThemeMode}
          accessibilityLabelPrefix="Theme"
        />
      </Section>

      <Section title="Defaults" hint="Applied to every new conversion.">
        <AppText variant="caption" tone="secondary">
          Default goal
        </AppText>
        <OptionChips
          options={GOAL_OPTIONS}
          selected={defaultGoal}
          onSelect={setDefaultGoal}
          accessibilityLabelPrefix="Default goal"
        />
        <AppText variant="caption" tone="secondary">
          Default output container
        </AppText>
        <OptionChips
          options={CONTAINER_OPTIONS}
          selected={defaultOutputContainer}
          onSelect={setDefaultOutputContainer}
          accessibilityLabelPrefix="Default output container"
        />
      </Section>

      <Section
        title="Processing preference"
        hint="A hint for the planner — the engine still decides per pipeline stage.">
        <OptionChips
          options={PROCESSING_OPTIONS}
          selected={processingPreference}
          onSelect={setProcessingPreference}
          accessibilityLabelPrefix="Processing preference"
        />
      </Section>

      <Section title="Recovery" hint="Interrupted jobs resume from checkpoints, not from zero.">
        <ToggleRow
          label="Auto-resume interrupted jobs on launch"
          value={autoResumeOnLaunch}
          onValueChange={setAutoResumeOnLaunch}
        />
        <ToggleRow
          label="Keep recovery segments after completion"
          value={keepRecoverySegments}
          onValueChange={setKeepRecoverySegments}
        />
      </Section>

      <Section title="Storage">
        <AppText variant="caption" tone="secondary">
          Output location
        </AppText>
        <AppText variant="caption" tone="primary">
          Outputs are written and managed by the Velmux engine. Originals are
          never replaced or deleted.
        </AppText>
      </Section>

      <Section title="Notifications">
        <ToggleRow
          label="Progress updates"
          value={notifyProgress}
          onValueChange={setNotifyProgress}
        />
        <ToggleRow
          label="Completion"
          value={notifyCompletion}
          onValueChange={setNotifyCompletion}
        />
        <ToggleRow
          label="Failures"
          value={notifyFailures}
          onValueChange={setNotifyFailures}
        />
      </Section>

      <Section
        title="Diagnostics"
        hint="Engine bridge and backend registry for this build.">
        <View style={styles.diagnosticsCard}>
          <View
            style={styles.diagnosticRow}
            accessible
            accessibilityLabel={`Engine bridge, ${
              bridgeAvailable ? 'Connected' : 'Not linked'
            }`}>
            <AppText variant="caption" tone="secondary">
              Engine bridge
            </AppText>
            <AppText
              variant="captionStrong"
              tone={bridgeAvailable ? 'success' : 'warning'}>
              {bridgeAvailable ? 'Connected' : 'Not linked'}
            </AppText>
          </View>
          {!bridgeAvailable && engineError !== null && (
            <View style={styles.bannerWrap}>
              <Banner message={engineError} tone="warning" />
            </View>
          )}
          <View style={styles.diagnosticRow}>
            <AppText variant="caption" tone="secondary">
              Backends
            </AppText>
          </View>
          {backends !== null && backends.length > 0 ? (
            <View style={styles.backendStack}>
              {backends.map(backend => (
                <AppText key={backend} variant="caption" numeric>
                  {backend}
                </AppText>
              ))}
            </View>
          ) : (
            <AppText variant="caption" tone="muted">
              {backendsError !== null
                ? backendsError
                : bridgeAvailable
                  ? 'No backends registered'
                  : 'Backend registry requires the native engine'}
            </AppText>
          )}
          <View style={styles.refreshRow}>
            <Button
              label="Re-check engine"
              variant="secondary"
              onPress={() => {
                discoverBackends();
              }}
            />
          </View>
        </View>
      </Section>
    </Screen>
  );
}

const styles = StyleSheet.create({
  diagnosticsCard: {
    gap: 8,
  },
  diagnosticRow: {
    minHeight: 32,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  bannerWrap: {
    marginVertical: 4,
  },
  backendStack: {
    gap: 2,
  },
  refreshRow: {
    marginTop: 4,
    alignSelf: 'flex-start',
  },
});
