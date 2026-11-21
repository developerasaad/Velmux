import React from 'react';
import {ActivityIndicator, StyleSheet, View} from 'react-native';

import {AppText} from '../../components/AppText';
import {ErrorState} from '../../components/ErrorState';
import {Section} from '../../components/Section';
import {formatBytes} from '../../format/format';
import {useTheme} from '../../theme';

export type PlanState =
  | {status: 'idle'}
  | {status: 'unavailable'; message: string}
  | {status: 'estimating'}
  | {status: 'error'; message: string}
  | {
      status: 'ready';
      estimatedOutputBytes: number | null;
      pipelineSummary: string;
      feasible: boolean;
      message: string | null;
    };

export interface EstimateCardProps {
  planState: PlanState;
  onRetry: () => void;
}

/**
 * Estimated output size, fed by `engine.plan()`. Shows an honest
 * "not enough information" state instead of an invented number.
 */
export function EstimateCard({planState, onRetry}: EstimateCardProps) {
  const theme = useTheme();

  let content: React.ReactNode;
  if (planState.status === 'idle' || planState.status === 'estimating') {
    content = (
      <View style={styles.loadingRow}>
        <ActivityIndicator size="small" color={theme.colors.accent} />
        <AppText variant="caption" tone="secondary">
          Estimating output size
        </AppText>
      </View>
    );
  } else if (planState.status === 'unavailable') {
    content = (
      <AppText variant="caption" tone="secondary">
        {planState.message}
      </AppText>
    );
  } else if (planState.status === 'error') {
    content = (
      <ErrorState
        title="Estimate unavailable"
        message={planState.message}
        retryLabel="Retry"
        onRetry={onRetry}
      />
    );
  } else {
    content = (
      <View style={styles.stack}>
        <View style={styles.sizeRow}>
          <AppText variant="caption" tone="secondary">
            Estimated output
          </AppText>
          {planState.estimatedOutputBytes === null ? (
            <AppText variant="bodyStrong" tone="secondary">
              Not enough information yet
            </AppText>
          ) : (
            <AppText
              variant="bodyStrong"
              numeric
              tone={planState.feasible ? 'primary' : 'warning'}>
              ~{formatBytes(planState.estimatedOutputBytes)}
            </AppText>
          )}
        </View>
        {planState.pipelineSummary.length > 0 && (
          <AppText variant="caption" tone="muted">
            {planState.pipelineSummary}
          </AppText>
        )}
        {planState.message !== null && (
          <AppText
            variant="caption"
            tone={planState.feasible ? 'secondary' : 'warning'}>
            {planState.message}
          </AppText>
        )}
      </View>
    );
  }

  return (
    <Section title="Estimated output" hint="From the engine plan for your current settings.">
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
});
