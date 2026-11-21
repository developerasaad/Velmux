import React from 'react';
import {ActivityIndicator, StyleSheet, View, ViewStyle} from 'react-native';

import {AppText} from '../../components/AppText';
import {ErrorState} from '../../components/ErrorState';
import {Section} from '../../components/Section';
import {
  formatBitRate,
  formatBytes,
  formatDuration,
  formatSampleRate,
} from '../../format/format';
import type {MediaInfo} from '../../native/NativeVelmux';
import {useTheme} from '../../theme';

export type InspectState =
  | {status: 'idle'}
  | {status: 'loading'}
  | {status: 'error'; message: string}
  | {status: 'unavailable'; message: string}
  | {status: 'ready'; info: MediaInfo};

export interface SourceInfoCardProps {
  sourceName: string;
  inspectState: InspectState;
  onRetry: () => void;
}

function channelsLabel(channels: number): string {
  if (channels === 1) {
    return 'mono';
  }
  if (channels === 2) {
    return 'stereo';
  }
  return `${channels} ch`;
}

/** Source summary fed by `engine.inspect()` — real states only. */
export function SourceInfoCard({
  sourceName,
  inspectState,
  onRetry,
}: SourceInfoCardProps) {
  const theme = useTheme();

  return (
    <Section title="Source" hint={sourceName}>
      {inspectState.status === 'idle' || inspectState.status === 'loading' ? (
        <View style={styles.loadingRow}>
          <ActivityIndicator size="small" color={theme.colors.accent} />
          <AppText variant="caption" tone="secondary">
            Inspecting source
          </AppText>
        </View>
      ) : inspectState.status === 'error' ? (
        <ErrorState
          title="Could not inspect this file"
          message={inspectState.message}
          retryLabel="Retry"
          onRetry={onRetry}
        />
      ) : inspectState.status === 'unavailable' ? (
        <AppText variant="caption" tone="secondary">
          {inspectState.message}
        </AppText>
      ) : (
        <View style={styles.infoStack}>
          <View style={styles.summaryRow}>
            <AppText variant="caption" tone="secondary">
              {inspectState.info.container}
            </AppText>
            <AppText variant="caption" tone="secondary" numeric>
              {formatBytes(inspectState.info.sizeBytes)}
            </AppText>
            <AppText variant="caption" tone="secondary" numeric>
              {formatDuration(inspectState.info.durationSec)}
            </AppText>
          </View>

          {inspectState.info.video.length === 0 ? (
            <AppText variant="caption" tone="muted">
              No video track
            </AppText>
          ) : (
            inspectState.info.video.map((stream, index) => (
              <AppText
                key={`video-${index}`}
                variant="caption"
                tone="primary"
                numeric>
                {`Video ${index + 1}: ${stream.codec} · ${stream.width}×${
                  stream.height
                } · ${Math.round(stream.frameRate)} fps · ${formatBitRate(
                  stream.bitRate,
                )} · ${stream.pixelFormat}${stream.hdr ? ' · HDR' : ''}`}
              </AppText>
            ))
          )}

          {inspectState.info.audio.map((stream, index) => (
            <AppText
              key={`audio-${index}`}
              variant="caption"
              tone="primary"
              numeric>
              {`Audio ${index + 1}: ${stream.codec} · ${channelsLabel(
                stream.channels,
              )} · ${formatSampleRate(stream.sampleRate)} · ${formatBitRate(
                stream.bitRate,
              )}${stream.language !== null ? ` · ${stream.language}` : ''}`}
            </AppText>
          ))}

          <AppText variant="caption" tone="muted" numeric>
            {`Subtitles: ${inspectState.info.subtitles.length}${
              inspectState.info.subtitles.length > 0
                ? ` (${inspectState.info.subtitles
                    .slice(0, 3)
                    .map(
                      track =>
                        `${track.format}${
                          track.language !== null ? ` ${track.language}` : ''
                        }`,
                    )
                    .join(', ')}${
                    inspectState.info.subtitles.length > 3 ? ', …' : ''
                  })`
                : ''
            }`}
          </AppText>
        </View>
      )}
    </Section>
  );
}

interface SourceInfoCardStyles {
  loadingRow: ViewStyle;
  infoStack: ViewStyle;
  summaryRow: ViewStyle;
}

const styles = StyleSheet.create<SourceInfoCardStyles>({
  loadingRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    minHeight: 44,
  },
  infoStack: {
    gap: 4,
  },
  summaryRow: {
    flexDirection: 'row',
    gap: 16,
    flexWrap: 'wrap',
  },
});
