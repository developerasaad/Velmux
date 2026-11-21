import React from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from './AppText';
import {ProgressBar} from './ProgressBar';
import {StateChip} from './Chip';
import {jobStateLabel, jobStateTone} from '../constants/jobStates';
import {formatBytes, formatPercent} from '../format/format';
import type {JobSnapshot} from '../native/NativeVelmux';
import {useTheme} from '../theme';

export interface JobRowProps {
  job: JobSnapshot;
  /** Compact rows are used in the Home "Active jobs" list. */
  compact?: boolean;
  /** Per-job actions rendered below the row content. */
  actions?: React.ReactNode;
}

/**
 * Job row with real state only: progress comes from the engine snapshot,
 * unknown progress renders "—", and no ETAs are ever invented.
 */
export function JobRow({job, compact = false, actions}: JobRowProps) {
  const theme = useTheme();
  const completed = job.state === 'completed';
  const knownProgress =
    Number.isFinite(job.progress) && job.progress >= 0 && job.progress <= 1;

  return (
    <View
      style={[
        styles.container,
        compact && styles.compact,
        {backgroundColor: theme.colors.surface},
      ]}>
      <View style={styles.titleRow}>
        <AppText
          variant={compact ? 'captionStrong' : 'bodyStrong'}
          style={styles.name}
          numberOfLines={1}>
          {job.sourceName}
        </AppText>
        <StateChip label={jobStateLabel(job.state)} tone={jobStateTone(job.state)} />
      </View>

      {completed ? (
        <AppText variant="caption" tone="secondary" numeric>
          {job.outputSizeBytes !== null
            ? `Output ${formatBytes(job.outputSizeBytes)}`
            : 'Output size unknown'}
        </AppText>
      ) : (
        <View style={styles.progressRow}>
          <ProgressBar progress={job.progress} />
          <AppText variant="caption" tone="secondary" numeric style={styles.percent}>
            {formatPercent(knownProgress ? job.progress : null)}
          </AppText>
        </View>
      )}

      {!compact && job.pipelineSummary !== null && (
        <AppText variant="caption" tone="muted" numberOfLines={2}>
          {job.pipelineSummary}
        </AppText>
      )}

      {!compact && job.state === 'failed' && job.error !== null && (
        <AppText variant="caption" tone="danger" style={styles.error}>
          {job.error}
        </AppText>
      )}

      {actions !== undefined && <View style={styles.actions}>{actions}</View>}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    padding: 12,
    gap: 8,
  },
  compact: {
    padding: 10,
    gap: 6,
  },
  titleRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  name: {
    flexShrink: 1,
  },
  progressRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  percent: {
    minWidth: 36,
    textAlign: 'right',
  },
  error: {
    marginTop: 0,
  },
  actions: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    marginTop: 4,
  },
});
