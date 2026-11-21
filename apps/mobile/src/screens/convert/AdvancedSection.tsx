import React, {useState} from 'react';
import {Pressable, StyleSheet, View, ViewStyle} from 'react-native';

import {AppText} from '../../components/AppText';
import {Button} from '../../components/Button';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {Stepper} from '../../components/Stepper';
import {
  AUDIO_BITRATES_KBPS,
  AUDIO_ENCODERS,
  BIT_DEPTHS,
  CHANNEL_OPTIONS,
  PIXEL_FORMATS,
  SAMPLE_RATES,
  VIDEO_ENCODERS,
  VIDEO_LEVELS,
  VIDEO_PROFILES,
} from '../../constants/options';
import {useConvertStore} from '../../state/convertStore';
import {computeGoalLabel} from '../../state/convertStore';
import type {
  AdvancedExtras,
  RateControl,
  SubtitlePolicy,
} from '../../state/convertStore';
import type {ProcessingPreference} from '../../state/settingsStore';

const VIDEO_CODEC_OPTIONS: ReadonlyArray<{label: string; value: string | null}> =
  [
    {label: 'Auto', value: null},
    ...VIDEO_ENCODERS.map(codec => ({label: codec, value: codec})),
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

const RATE_CONTROL_OPTIONS: ReadonlyArray<{
  label: string;
  value: RateControl;
}> = [
  {label: 'Auto', value: 'auto'},
  {label: 'Bitrate', value: 'bitrate'},
  {label: 'Quality', value: 'quality'},
];

const AUDIO_CODEC_OPTIONS: ReadonlyArray<{label: string; value: string | null}> =
  [
    {label: 'Auto', value: null},
    ...AUDIO_ENCODERS.map(codec => ({label: codec, value: codec})),
  ];

const SAMPLE_RATE_OPTIONS = SAMPLE_RATES.map(rate => ({
  label: rate.label,
  value: rate.value,
}));

const CHANNEL_LAYOUT_OPTIONS = CHANNEL_OPTIONS.map(channel => ({
  label: channel.label,
  value: channel.value,
}));

const PROFILE_OPTIONS: ReadonlyArray<{label: string; value: string | null}> = [
  {label: 'Auto', value: null},
  ...VIDEO_PROFILES.filter(profile => profile !== 'Auto').map(profile => ({
    label: profile,
    value: profile,
  })),
];

const LEVEL_OPTIONS: ReadonlyArray<{label: string; value: string | null}> = [
  {label: 'Auto', value: null},
  ...VIDEO_LEVELS.filter(level => level !== 'Auto').map(level => ({
    label: level,
    value: level,
  })),
];

const PIXEL_FORMAT_OPTIONS: ReadonlyArray<{label: string; value: string | null}> =
  [
    {label: 'Auto', value: null},
    ...PIXEL_FORMATS.filter(format => format !== 'Auto').map(format => ({
      label: format,
      value: format,
    })),
  ];

const BIT_DEPTH_OPTIONS: ReadonlyArray<{label: string; value: number | null}> = [
  {label: 'Auto', value: null},
  ...BIT_DEPTHS.map(depth => ({label: `${depth}-bit`, value: depth})),
];

const METADATA_OPTIONS: ReadonlyArray<{
  label: string;
  value: AdvancedExtras['metadata'];
}> = [
  {label: 'Preserve', value: 'preserve'},
  {label: 'Strip', value: 'strip'},
];

const SUBTITLE_BEHAVIOR_OPTIONS: ReadonlyArray<{
  label: string;
  value: SubtitlePolicy;
}> = [
  {label: 'Preserve', value: 'preserve'},
  {label: 'Remove', value: 'remove'},
  {label: 'Extract', value: 'extract'},
  {label: 'Burn in', value: 'burnIn'},
];

function AdvancedLabel({children}: {children: string}) {
  return (
    <AppText variant="caption" tone="secondary">
      {children}
    </AppText>
  );
}

/**
 * Advanced technical controls. Hidden by default; closing the section
 * never discards changes — all values live in the convert store.
 *
 * Fields marked "pending contract" are part of the product surface but not
 * yet part of the `ConversionRequest` bridge contract; they are kept in
 * the draft until the engine contract is extended.
 */
export function AdvancedSection() {
  const [open, setOpen] = useState(false);

  const draft = useConvertStore(state => state.draft);
  const setVideoCodec = useConvertStore(state => state.setVideoCodec);
  const setFrameRate = useConvertStore(state => state.setFrameRate);
  const setRateControl = useConvertStore(state => state.setRateControl);
  const setVideoBitRate = useConvertStore(state => state.setVideoBitRate);
  const setVideoQuality = useConvertStore(state => state.setVideoQuality);
  const setAudioCodec = useConvertStore(state => state.setAudioCodec);
  const setAudioBitRate = useConvertStore(state => state.setAudioBitRate);
  const setAudioSampleRate = useConvertStore(state => state.setAudioSampleRate);
  const setAudioChannels = useConvertStore(state => state.setAudioChannels);
  const setSubtitlePolicy = useConvertStore(state => state.setSubtitlePolicy);
  const setTimingOffsetMs = useConvertStore(state => state.setTimingOffsetMs);
  const setProcessingMode = useConvertStore(state => state.setProcessingMode);
  const setExtras = useConvertStore(state => state.setExtras);
  const resetAdvanced = useConvertStore(state => state.resetAdvanced);

  return (
    <Section
      title="Advanced"
      hint="Exact technical controls. Closing this section keeps your changes."
      action={
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={open ? 'Hide advanced controls' : 'Show advanced controls'}
          onPress={() => setOpen(previous => !previous)}
          style={({pressed}) => [styles.toggle, pressed && styles.pressed]}>
          <AppText variant="captionStrong" tone="accent">
            {open ? 'Hide' : 'Show'}
          </AppText>
        </Pressable>
      }>
      {open && (
        <View style={styles.stack}>
          {draft.advancedDirty && (
            <AppText variant="micro" tone="warning">
              {`Goal relabeled to ${computeGoalLabel(draft)}.`}
            </AppText>
          )}

          <AdvancedLabel>Video codec</AdvancedLabel>
          <OptionChips
            options={VIDEO_CODEC_OPTIONS}
            selected={draft.video.codec}
            onSelect={setVideoCodec}
          />

          <AdvancedLabel>Encoder / backend preference</AdvancedLabel>
          <OptionChips
            options={PROCESSING_OPTIONS}
            selected={draft.processingMode}
            onSelect={setProcessingMode}
          />

          <AdvancedLabel>Rate control</AdvancedLabel>
          <OptionChips
            options={RATE_CONTROL_OPTIONS}
            selected={draft.video.rateControl}
            onSelect={setRateControl}
          />

          {draft.video.rateControl === 'bitrate' && (
            <View style={styles.field}>
              <AdvancedLabel>Video bitrate</AdvancedLabel>
              <Stepper
                value={draft.video.bitRate}
                onChange={setVideoBitRate}
                step={500_000}
                min={100_000}
                max={200_000_000}
                initialWhenNull={4_000_000}
                format={value => `${(value / 1_000_000).toFixed(1)} Mb/s`}
              />
            </View>
          )}

          {draft.video.rateControl === 'quality' && (
            <View style={styles.field}>
              <AdvancedLabel>Quality parameter</AdvancedLabel>
              <Stepper
                value={draft.video.quality}
                onChange={setVideoQuality}
                step={5}
                min={0}
                max={100}
                initialWhenNull={60}
              />
            </View>
          )}

          <View style={styles.field}>
            <AdvancedLabel>Frame rate</AdvancedLabel>
            <Stepper
              value={draft.video.frameRate}
              onChange={setFrameRate}
              step={1}
              min={1}
              max={240}
              initialWhenNull={30}
              nullLabel="Original"
              format={value => `${Math.round(value)} fps`}
            />
          </View>

          <AdvancedLabel>Profile (pending contract)</AdvancedLabel>
          <OptionChips
            options={PROFILE_OPTIONS}
            selected={draft.extras.profile}
            onSelect={profile => setExtras({profile})}
          />

          <AdvancedLabel>Level (pending contract)</AdvancedLabel>
          <OptionChips
            options={LEVEL_OPTIONS}
            selected={draft.extras.level}
            onSelect={level => setExtras({level})}
          />

          <AdvancedLabel>Pixel format (pending contract)</AdvancedLabel>
          <OptionChips
            options={PIXEL_FORMAT_OPTIONS}
            selected={draft.extras.pixelFormat}
            onSelect={pixelFormat => setExtras({pixelFormat})}
          />

          <AdvancedLabel>Bit depth (pending contract)</AdvancedLabel>
          <OptionChips
            options={BIT_DEPTH_OPTIONS}
            selected={draft.extras.bitDepth}
            onSelect={bitDepth => setExtras({bitDepth})}
          />

          <View style={styles.field}>
            <AdvancedLabel>GOP size (pending contract)</AdvancedLabel>
            <Stepper
              value={draft.extras.gopSize}
              onChange={gopSize => setExtras({gopSize})}
              step={12}
              min={1}
              max={600}
              initialWhenNull={48}
              format={value => `${Math.round(value)} frames`}
            />
          </View>

          <View style={styles.field}>
            <AdvancedLabel>B-frames (pending contract)</AdvancedLabel>
            <Stepper
              value={draft.extras.bFrames}
              onChange={bFrames => setExtras({bFrames})}
              step={1}
              min={0}
              max={8}
              initialWhenNull={2}
            />
          </View>

          <AdvancedLabel>Audio codec</AdvancedLabel>
          <OptionChips
            options={AUDIO_CODEC_OPTIONS}
            selected={draft.audio.codec}
            onSelect={setAudioCodec}
          />

          <View style={styles.field}>
            <AdvancedLabel>Audio bitrate</AdvancedLabel>
            <Stepper
              value={draft.audio.bitRate}
              onChange={setAudioBitRate}
              step={8_000}
              min={8_000}
              max={320_000}
              initialWhenNull={128_000}
              format={value => `${Math.round(value / 1000)} kb/s`}
            />
          </View>

          <AdvancedLabel>Sample rate</AdvancedLabel>
          <OptionChips
            options={SAMPLE_RATE_OPTIONS}
            selected={draft.audio.sampleRate}
            onSelect={setAudioSampleRate}
          />

          <AdvancedLabel>Channel layout</AdvancedLabel>
          <OptionChips
            options={CHANNEL_LAYOUT_OPTIONS}
            selected={draft.audio.channels}
            onSelect={setAudioChannels}
          />

          <AdvancedLabel>Subtitle behavior</AdvancedLabel>
          <OptionChips
            options={SUBTITLE_BEHAVIOR_OPTIONS}
            selected={draft.subtitles.policy}
            onSelect={setSubtitlePolicy}
          />

          <View style={styles.field}>
            <AdvancedLabel>Subtitle timing offset</AdvancedLabel>
            <Stepper
              value={draft.subtitles.timingOffsetMs}
              onChange={setTimingOffsetMs}
              step={100}
              min={-10_000}
              max={10_000}
              format={value => `${value > 0 ? '+' : ''}${Math.round(value)} ms`}
            />
          </View>

          <AdvancedLabel>Metadata behavior (pending contract)</AdvancedLabel>
          <OptionChips
            options={METADATA_OPTIONS}
            selected={draft.extras.metadata}
            onSelect={metadata => setExtras({metadata})}
          />

          <View style={styles.resetRow}>
            <Button
              label="Reset to preset defaults"
              variant="ghost"
              onPress={resetAdvanced}
              disabled={!draft.advancedDirty}
            />
          </View>

          <AppText variant="micro" tone="muted" style={styles.footnote}>
            Fields marked "pending contract" are kept with your settings and
            will be sent to the engine once the bridge contract exposes them.
          </AppText>
        </View>
      )}
      {!open && draft.advancedDirty && (
        <AppText variant="caption" tone="warning">
          Custom values are set. Show to review them.
        </AppText>
      )}
    </Section>
  );
}

interface AdvancedSectionStyles {
  toggle: ViewStyle;
  stack: ViewStyle;
  field: ViewStyle;
  resetRow: ViewStyle;
  footnote: ViewStyle;
  pressed: ViewStyle;
}

const styles = StyleSheet.create<AdvancedSectionStyles>({
  toggle: {
    minHeight: 44,
    justifyContent: 'center',
  },
  stack: {
    gap: 12,
  },
  field: {
    gap: 8,
  },
  resetRow: {
    marginTop: 4,
  },
  footnote: {
    marginTop: 4,
  },
  pressed: {
    opacity: 0.7,
  },
});
