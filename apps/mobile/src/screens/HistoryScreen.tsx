import React, {useCallback} from 'react';
import {useShallow} from 'zustand/react/shallow';
import {FlatList, Platform, RefreshControl, StyleSheet, View} from 'react-native';
import type {ListRenderItemInfo} from 'react-native';
import {
  useNavigation,
  type CompositeScreenProps,
} from '@react-navigation/native';
import type {BottomTabScreenProps} from '@react-navigation/bottom-tabs';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';

import {AppText} from '../components/AppText';
import {Button} from '../components/Button';
import {EmptyState} from '../components/EmptyState';
import {OutputActions} from '../components/OutputActions';
import {Screen} from '../components/Screen';
import {formatBytes, formatSavedPercent} from '../format/format';
import type {JobSnapshot} from '../native/NativeVelmux';
import {
  selectCompletedJobs,
  useJobsStore,
} from '../state/jobsStore';
import {useTheme} from '../theme';
import type {RootStackParamList, TabParamList} from '../app/types';

type HistoryScreenProps = CompositeScreenProps<
  BottomTabScreenProps<TabParamList, 'History'>,
  NativeStackScreenProps<RootStackParamList>
>;

/**
 * Completed conversions. Output actions read the engine-reported output
 * location (`outputPath` in the job snapshot); see OutputActions for the
 * per-platform story. "Reuse settings" works for jobs started in this JS
 * session (their request is kept in memory).
 */
export function HistoryScreen() {
  const theme = useTheme();
  const navigation = useNavigation<HistoryScreenProps['navigation']>();

  // useShallow for the array-returning selector (fresh .filter/.reverse
  // references would loop useSyncExternalStore — see HomeScreen).
  const completedJobs = useJobsStore(useShallow(selectCompletedJobs));
  const requests = useJobsStore(state => state.requests);
  const clearBatch = useJobsStore(state => state.clearBatch);
  const refreshing = useJobsStore(state => state.refreshing);
  const refresh = useJobsStore(state => state.refresh);

  const renderItem = useCallback(
    ({item}: ListRenderItemInfo<JobSnapshot>) => {
      const request = requests[item.id];
      const canReuse = request !== undefined;
      // One screen-reader element per completed conversion: name, sizes,
      // saved space and the pipeline summary read as a single row.
      const rowLabel = [
        item.sourceName,
        `${formatBytes(item.inputSizeBytes)} to ${formatBytes(
          item.outputSizeBytes,
        )}`,
        `Saved ${formatSavedPercent(item.inputSizeBytes, item.outputSizeBytes)}`,
        item.pipelineSummary ?? undefined,
      ]
        .filter(part => part !== undefined)
        .join(', ');
      return (
        <View
          style={[styles.row, {backgroundColor: theme.colors.surface}]}>
          <View accessible accessibilityLabel={rowLabel} style={styles.infoGroup}>
            <AppText variant="bodyStrong" numberOfLines={1}>
              {item.sourceName}
            </AppText>
            <View style={styles.sizeRow}>
              <AppText variant="caption" tone="secondary" numeric>
                {formatBytes(item.inputSizeBytes)} →{' '}
                {formatBytes(item.outputSizeBytes)}
              </AppText>
              <AppText variant="caption" tone="success" numeric>
                Saved{' '}
                {formatSavedPercent(item.inputSizeBytes, item.outputSizeBytes)}
              </AppText>
            </View>
            {item.pipelineSummary !== null && (
              <AppText variant="caption" tone="muted" numberOfLines={2}>
                {item.pipelineSummary}
              </AppText>
            )}
          </View>
          <View style={styles.actions}>
            <OutputActions job={item} />
            <Button
              label="Reuse settings"
              variant="secondary"
              disabled={!canReuse}
              accessibilityLabel={`Reuse settings for ${item.sourceName}`}
              onPress={() => {
                if (!request) {
                  return;
                }
                // Entering the workspace from History is a single-file
                // intent — never piggyback on a staged Home batch.
                clearBatch();
                navigation.navigate('Convert', {
                  source: request.source,
                  intent: 'reuse',
                  request,
                });
              }}
            />
          </View>
        </View>
      );
    },
    [clearBatch, navigation, requests, theme.colors.surface],
  );

  const empty = completedJobs.length === 0;

  return (
    <Screen
      title="History"
      subtitle="Completed conversions with space saved and output summary.">
      <FlatList
        data={completedJobs}
        keyExtractor={item => item.id}
        renderItem={renderItem}
        contentContainerStyle={
          empty ? styles.emptyContent : styles.listContent
        }
        refreshControl={
          <RefreshControl
            refreshing={refreshing}
            onRefresh={() => {
              refresh();
            }}
            tintColor={theme.colors.textSecondary}
            colors={[theme.colors.accent]}
          />
        }
        ListEmptyComponent={
          <View style={styles.emptyWrap}>
            <EmptyState
              title="No completed conversions yet"
              message="Finished jobs appear here — original size, output size and space saved."
            />
          </View>
        }
        ListFooterComponent={
          completedJobs.length > 0 ? (
            <View style={styles.footerNote}>
              <AppText variant="micro" tone="muted">
                {Platform.OS === 'ios'
                  ? 'Share opens the system share sheet with the converted file. Remove clears the history record — the output file itself is kept.'
                  : 'Android requires a content provider before other apps can receive the output file, which this build does not wire up yet — Share arrives with it. Remove clears the history record; the output file is kept.'}
              </AppText>
            </View>
          ) : undefined
        }
      />
    </Screen>
  );
}

const styles = StyleSheet.create({
  row: {
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    padding: 12,
    gap: 8,
  },
  infoGroup: {
    gap: 8,
  },
  sizeRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
  actions: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 8,
    marginTop: 4,
  },
  listContent: {
    padding: 16,
    gap: 12,
    paddingBottom: 32,
  },
  emptyContent: {
    flex: 1,
    paddingHorizontal: 16,
  },
  emptyWrap: {
    flex: 1,
    justifyContent: 'center',
  },
  footerNote: {
    paddingTop: 12,
  },
});
