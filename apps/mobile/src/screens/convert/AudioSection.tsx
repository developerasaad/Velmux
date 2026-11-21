import React from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from '../../components/AppText';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {ToggleRow} from '../../components/ToggleRow';
import {
  AUDIO_BITRATES_KBPS,
  AUDIO_ENCODERS,
  CHANNEL_OPTIONS,
  isAudioOnlyContainer,
  SAMPLE_RATES,
} from '../../constants/options';
import {useConvertStore} from '../../state/convertStore';

const CODEC_OPTIONS: ReadonlyArray<{label: string; value: string | null}> = [
  {label: 'Auto', value: null},
  ...AUDIO_ENCODERS.map(codec => ({label: codec, value: codec})),
];

const BITRATE_OPTIONS: ReadonlyArray<{label: string; value: number | null}> = [
  {label: 'Auto', value: null},
  ...AUDIO_BITRATES_KBPS.map(kbps => ({
    label: `${kbps} kbps`,
    value: kbps * 1000,
  })),
];

const SAMPLE_RATE_OPTIONS = SAMPLE_RATES.map(rate => ({
  label: rate.label,
  value: rate.value,
}));

const CHANNEL_LAYOUT_OPTIONS = CHANNEL_OPTIONS.map(channel => ({
  label: channel.label,
  value: channel.value,
}));

/** Audio track handling. */
export function AudioSection() {
  const draft = useConvertStore(state => state.draft);
  const setAudioEnabled = useConvertStore(state => state.setAudioEnabled);
  const setAudioCodec = useConvertStore(state => state.setAudioCodec);
  const setAudioBitRate = useConvertStore(state => state.setAudioBitRate);
  const setAudioSampleRate = useConvertStore(state => state.setAudioSampleRate);
  const setAudioChannels = useConvertStore(state => state.setAudioChannels);

  const audioOnlyOutput = isAudioOnlyContainer(draft.outputContainer);

  return (
    <Section
      title="Audio"
      hint={
        draft.audio.enabled
          ? undefined
          : 'Audio will be removed from the output.'
      }>
      <View style={styles.toggleBlock}>
        <ToggleRow
          label="Include audio"
          value={draft.audio.enabled}
          onValueChange={setAudioEnabled}
          disabled={audioOnlyOutput}
        />
        {audioOnlyOutput && (
          <AppText variant="micro" tone="muted">
            Audio-only formats always include the audio track.
          </AppText>
        )}
      </View>

      {draft.audio.enabled && (
        <View style={styles.controls}>
          <AppText variant="caption" tone="secondary">
            Codec
          </AppText>
          <OptionChips
            options={CODEC_OPTIONS}
            selected={draft.audio.codec}
            onSelect={setAudioCodec}
          />

          <AppText variant="caption" tone="secondary">
            Bitrate
          </AppText>
          <OptionChips
            options={BITRATE_OPTIONS}
            selected={draft.audio.bitRate}
            onSelect={setAudioBitRate}
          />

          <AppText variant="caption" tone="secondary">
            Sample rate
          </AppText>
          <OptionChips
            options={SAMPLE_RATE_OPTIONS}
            selected={draft.audio.sampleRate}
            onSelect={setAudioSampleRate}
          />

          <AppText variant="caption" tone="secondary">
            Channels
          </AppText>
          <OptionChips
            options={CHANNEL_LAYOUT_OPTIONS}
            selected={draft.audio.channels}
            onSelect={setAudioChannels}
          />
        </View>
      )}
    </Section>
  );
}

const styles = StyleSheet.create({
  toggleBlock: {
    gap: 4,
  },
  controls: {
    gap: 12,
    marginTop: 4,
  },
});
