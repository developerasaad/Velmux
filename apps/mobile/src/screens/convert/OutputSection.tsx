import React from 'react';
import {StyleSheet, View, ViewStyle} from 'react-native';

import {AppText} from '../../components/AppText';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {TextField} from '../../components/TextField';
import {ToggleRow} from '../../components/ToggleRow';
import {CONTAINERS, isAudioOnlyContainer, RESOLUTIONS} from '../../constants/options';
import {useConvertStore} from '../../state/convertStore';
import type {ResolutionOption} from '../../constants/options';

const CONTAINER_OPTIONS = CONTAINERS.map(container => ({
  label: container.label,
  value: container.id,
}));

const RESOLUTION_OPTIONS: ReadonlyArray<{label: string; value: string}> =
  RESOLUTIONS.map(resolution => ({
    label: resolution.label,
    value: resolution.id,
  }));

/** Output format and resolution. Only valid combinations are exposed. */
export function OutputSection() {
  const draft = useConvertStore(state => state.draft);
  const setOutputContainer = useConvertStore(state => state.setOutputContainer);
  const setResolutionChoice = useConvertStore(
    state => state.setResolutionChoice,
  );
  const setCustomResolutionText = useConvertStore(
    state => state.setCustomResolutionText,
  );
  const setVideoEnabled = useConvertStore(state => state.setVideoEnabled);

  const audioOnlyOutput = isAudioOnlyContainer(draft.outputContainer);

  return (
    <>
      <Section
        title="Output format"
        hint={
          audioOnlyOutput
            ? 'Audio-only format — the video track will not be included.'
            : undefined
        }>
        <OptionChips
          options={CONTAINER_OPTIONS}
          selected={draft.outputContainer ?? ''}
          onSelect={setOutputContainer}
        />
        <View style={styles.videoToggle}>
          <ToggleRow
            label="Include video"
            value={draft.video.enabled}
            onValueChange={setVideoEnabled}
            disabled={audioOnlyOutput}
          />
          {audioOnlyOutput && (
            <AppText variant="micro" tone="muted">
              Disabled for audio-only output formats.
            </AppText>
          )}
        </View>
      </Section>

      <Section title="Resolution">
        <OptionChips
          options={RESOLUTION_OPTIONS}
          selected={draft.resolutionChoice}
          onSelect={value =>
            setResolutionChoice(value as ResolutionOption['id'])
          }
          disabled={!draft.video.enabled}
        />
        {draft.resolutionChoice === 'custom' && (
          <View style={styles.customResolution}>
            <TextField
              label="Height (px)"
              value={draft.customResolutionText}
              onChangeText={setCustomResolutionText}
              placeholder="e.g. 854"
              keyboardType="number-pad"
              hint="Width follows the source aspect ratio."
            />
          </View>
        )}
        {!draft.video.enabled && (
          <AppText variant="micro" tone="muted">
            Resolution applies to the video track, which is currently off.
          </AppText>
        )}
      </Section>
    </>
  );
}

interface OutputSectionStyles {
  videoToggle: ViewStyle;
  customResolution: ViewStyle;
}

const styles = StyleSheet.create<OutputSectionStyles>({
  videoToggle: {
    marginTop: 4,
    gap: 4,
  },
  customResolution: {
    marginTop: 12,
  },
});
