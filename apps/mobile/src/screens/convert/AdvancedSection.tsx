import React, {useState} from 'react';
import {Pressable, StyleSheet, View} from 'react-native';

import {AppText} from '../../components/AppText';
import {Button} from '../../components/Button';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {Stepper} from '../../components/Stepper';
import {
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
            accessibilityLabelPrefix="Video codec"
          />

          <AdvancedLabel>Encoder / backend preference</AdvancedLabel>
          <OptionChips
            options={PROCESSING_OPTIONS}
            selected={draft.processingMode}
            onSelect={setProcessingMode}
            accessibilityLabelPrefix="Encoder preference"
          />

          <AdvancedLabel>Rate control</AdvancedLabel>
          <OptionChips
            options={RATE_CONTROL_OPTIONS}
            selected={draft.video.rateControl}
            onSelect={setRateControl}
            accessibilityLabelPrefix="Rate control"
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
                accessibilityLabel="video bitrate"
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
                accessibilityLabel="quality"
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
              accessibilityLabel="frame rate"
            />
          </View>

          <AdvancedLabel>Profile</AdvancedLabel>
          <OptionChips
            options={PROFILE_OPTIONS}
            selected={draft.extras.profile}
            onSelect={profile => setExtras({profile})}
            accessibilityLabelPrefix="Video profile"
          />

          <AdvancedLabel>Level</AdvancedLabel>
          <OptionChips
            options={LEVEL_OPTIONS}
            selected={draft.extras.level}
            onSelect={level => setExtras({level})}
            accessibilityLabelPrefix="Video level"
          />

          <AdvancedLabel>Pixel format</AdvancedLabel>
          <OptionChips
            options={PIXEL_FORMAT_OPTIONS}
            selected={draft.extras.pixelFormat}
            onSelect={pixelFormat => setExtras({pixelFormat})}
            accessibilityLabelPrefix="Pixel format"
          />

          <AdvancedLabel>Bit depth</AdvancedLabel>
          <OptionChips
            options={BIT_DEPTH_OPTIONS}
            selected={draft.extras.bitDepth}
            onSelect={bitDepth => setExtras({bitDepth})}
            accessibilityLabelPrefix="Bit depth"
          />

          <View style={styles.field}>
            <AdvancedLabel>GOP size</AdvancedLabel>
            <Stepper
              value={draft.extras.gopSize}
              onChange={gopSize => setExtras({gopSize})}
              step={12}
              min={1}
              max={600}
              initialWhenNull={48}
              format={value => `${Math.round(value)} frames`}
              accessibilityLabel="GOP size"
            />
          </View>

          <View style={styles.field}>
            <AdvancedLabel>B-frames</AdvancedLabel>
            <Stepper
              value={draft.extras.bFrames}
              onChange={bFrames => setExtras({bFrames})}
              step={1}
              min={0}
              max={8}
              initialWhenNull={2}
              accessibilityLabel="B-frames"
            />
          </View>

          <AdvancedLabel>Audio codec</AdvancedLabel>
          <OptionChips
            options={AUDIO_CODEC_OPTIONS}
            selected={draft.audio.codec}
            onSelect={setAudioCodec}
            accessibilityLabelPrefix="Audio codec"
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
              accessibilityLabel="audio bitrate"
            />
          </View>

          <AdvancedLabel>Sample rate</AdvancedLabel>
          <OptionChips
            options={SAMPLE_RATE_OPTIONS}
            selected={draft.audio.sampleRate}
            onSelect={setAudioSampleRate}
            accessibilityLabelPrefix="Audio sample rate"
          />

          <AdvancedLabel>Channel layout</AdvancedLabel>
          <OptionChips
            options={CHANNEL_LAYOUT_OPTIONS}
            selected={draft.audio.channels}
            onSelect={setAudioChannels}
            accessibilityLabelPrefix="Audio channel layout"
          />

          <AdvancedLabel>Subtitle behavior</AdvancedLabel>
          <OptionChips
            options={SUBTITLE_BEHAVIOR_OPTIONS}
            selected={draft.subtitles.policy}
            onSelect={setSubtitlePolicy}
            accessibilityLabelPrefix="Subtitle behavior"
          />

          <View style={styles.field}>
            <AdvancedLabel>Subtitle timing offset</AdvancedLabel>
            <Stepper
              value={draft.subtitles.timingOffsetMs}
              onChange={value => setTimingOffsetMs(value ?? 0)}
              step={100}
              min={-10_000}
              max={10_000}
              format={value => `${value > 0 ? '+' : ''}${Math.round(value)} ms`}
              accessibilityLabel="subtitle timing offset"
            />
          </View>

          <AdvancedLabel>Metadata behavior</AdvancedLabel>
          <OptionChips
            options={METADATA_OPTIONS}
            selected={draft.extras.metadata}
            onSelect={metadata => setExtras({metadata})}
            accessibilityLabelPrefix="Metadata behavior"
          />

          <View style={styles.resetRow}>
            <Button
              label="Reset to preset defaults"
              variant="ghost"
              onPress={resetAdvanced}
              disabled={!draft.advancedDirty}
            />
          </View>
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

const styles = StyleSheet.create({
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
  pressed: {
    opacity: 0.7,
  },
});
