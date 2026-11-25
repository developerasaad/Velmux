import React, {useCallback} from 'react';
import {ActivityIndicator, StyleSheet, View} from 'react-native';

import {AppText} from '../../components/AppText';
import {Button} from '../../components/Button';
import {ErrorState} from '../../components/ErrorState';
import {Section} from '../../components/Section';
import {formatBytes} from '../../format/format';
import {shareOutput} from '../../native/fileAccess';
import type {SampleResult} from '../../native/engineClient';
import {useTheme} from '../../theme';

/** Product default for sample previews: the densest 8 seconds (spec §20). */
export const SAMPLE_PREVIEW_SECONDS = 8;

export type SampleState =
  | {status: 'idle'}
  | {status: 'unavailable'; message: string}
  | {status: 'generating'}
  | {status: 'error'; message: string}
  | {status: 'ready'; sample: SampleResult};

export interface SamplePreviewCardProps {
  sampleState: SampleState;
  onGenerate: () => void;
}

/**
 * Sample compression preview (spec §20): a short sample generated over
 * the densest section of the source with the exact request the Convert
 * action would send, so size and quality can be checked before starting
 * the full conversion.
 *
 * Sample file actions reuse the OutputActions (History) platform story:
 * the platform share (iOS share sheet with the file:// URL; Android's
 * FileProvider path through the engine module), and no separate Play
 * control is shipped because RN core has no honest file-play API on
 * either platform.
 */
export function SamplePreviewCard({
  sampleState,
  onGenerate,
}: SamplePreviewCardProps) {
  const theme = useTheme();

  // Defensive against malformed engine results: an absent path simply
  // disables the share action instead of crashing on it.
  const samplePath =
    sampleState.status === 'ready' &&
    typeof sampleState.sample.path === 'string' &&
    sampleState.sample.path.length > 0
      ? sampleState.sample.path
      : null;

  const shareSample = useCallback(async () => {
    if (samplePath === null) {
      return;
    }
    // Platform share (core Share with the file:// URL on iOS; the
    // FileProvider path through the engine module on Android). The
    // basename carries the extension, so Android resolves the MIME type
    // for the share targets.
    const sampleName = samplePath.slice(samplePath.lastIndexOf('/') + 1);
    await shareOutput(samplePath, sampleName);
  }, [samplePath]);

  let content: React.ReactNode;
  if (sampleState.status === 'generating') {
    content = (
      <View style={styles.loadingRow}>
        <ActivityIndicator size="small" color={theme.colors.accent} />
        <AppText variant="caption" tone="secondary">
          {`Generating an ${SAMPLE_PREVIEW_SECONDS}-second sample`}
        </AppText>
      </View>
    );
  } else if (sampleState.status === 'unavailable') {
    content = (
      <AppText variant="caption" tone="secondary">
        {sampleState.message}
      </AppText>
    );
  } else if (sampleState.status === 'error') {
    content = (
      <ErrorState
        title="Could not generate the sample"
        message={sampleState.message}
        retryLabel="Retry"
        onRetry={onGenerate}
      />
    );
  } else if (sampleState.status === 'ready') {
    const sampleSize = formatBytes(sampleState.sample.sizeBytes);
    content = (
      <View style={styles.stack}>
        <View
          style={styles.sizeRow}
          accessible
          accessibilityLabel={`Sample size, ${sampleSize}`}>
          <AppText variant="caption" tone="secondary">
            Sample size
          </AppText>
          <AppText variant="bodyStrong" numeric>
            {sampleSize}
          </AppText>
        </View>
        <AppText variant="caption" tone="muted">
          {`The densest ${SAMPLE_PREVIEW_SECONDS} seconds of the source at your current settings.`}
        </AppText>
        <View style={styles.actions}>
          <Button
            label="Share sample"
            variant="secondary"
            disabled={samplePath === null}
            onPress={() => {
              shareSample();
            }}
          />
        </View>
      </View>
    );
  } else {
    content = (
      <Button
        label="Preview with a sample"
        variant="secondary"
        onPress={onGenerate}
      />
    );
  }

  return (
    <Section
      title="Sample preview"
      hint="Check quality and size with a short sample before converting.">
      {content}
    </Section>
  );
}

const styles = StyleSheet.create({
  loadingRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    minHeight: 44,
  },
  stack: {
    gap: 4,
  },
  sizeRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
  actions: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    marginTop: 8,
  },
});
