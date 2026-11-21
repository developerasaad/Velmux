import React, {useCallback} from 'react';
import {Alert, FlatList, RefreshControl, StyleSheet, View} from 'react-native';
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
 * Output file actions (Open, Play, Share, Delete) require the output file
 * location, which the current engine contract does not expose in
 * `JobSnapshot`. They are rendered but disabled until the contract is
 * extended; Reuse settings works for jobs started in this JS session.
 */
export function HistoryScreen() {
  const theme = useTheme();
  const navigation = useNavigation<HistoryScreenProps['navigation']>();

  const completedJobs = useJobsStore(selectCompletedJobs);
  const requests = useJobsStore(state => state.requests);
  const refreshing = useJobsStore(state => state.refreshing);
  const refresh = useJobsStore(state => state.refresh);

  const confirmDelete = useCallback((job: JobSnapshot) => {
    Alert.alert(
      'Delete output',
      `Delete the converted output of "${job.sourceName}"? The original file is never touched.`,
      [
        {text: 'Keep output', style: 'cancel'},
        {
          text: 'Delete output',
          style: 'destructive',
          onPress: () => {
            // Requires the output file location from the engine contract.
          },
        },
      ],
    );
  }, []);

  const renderItem = useCallback(
    ({item}: ListRenderItemInfo<JobSnapshot>) => {
      const request = requests[item.id];
      const canReuse = request !== undefined;
      return (
        <View
          style={[styles.row, {backgroundColor: theme.colors.surface}]}>
          <AppText variant="bodyStrong" numberOfLines={1}>
            {item.sourceName}
          </AppText>
          <View style={styles.sizeRow}>
            <AppText variant="caption" tone="secondary" numeric>
              {formatBytes(item.inputSizeBytes)} →{' '}
              {formatBytes(item.outputSizeBytes)}
            </AppText>
            <AppText variant="caption" tone="success" numeric>
              Saved {formatSavedPercent(item.inputSizeBytes, item.outputSizeBytes)}
            </AppText>
          </View>
          {item.pipelineSummary !== null && (
            <AppText variant="caption" tone="muted" numberOfLines={2}>
              {item.pipelineSummary}
            </AppText>
          )}
          <View style={styles.actions}>
            <Button label="Open" variant="secondary" disabled onPress={() => {}} />
            <Button label="Play" variant="secondary" disabled onPress={() => {}} />
            <Button label="Share" variant="secondary" disabled onPress={() => {}} />
            <Button
              label="Reuse settings"
              variant="secondary"
              disabled={!canReuse}
              onPress={() => {
                if (!request) {
                  return;
                }
                navigation.navigate('Convert', {
                  source: request.source,
                  intent: 'reuse',
                  request,
                });
              }}
            />
            <Button
              label="Delete"
              variant="dangerGhost"
              disabled
              onPress={() => confirmDelete(item)}
            />
          </View>
        </View>
      );
    },
    [confirmDelete, navigation, requests, theme.colors.surface],
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
                Open, Play, Share and Delete need the output file location,
                which the engine does not expose yet. Reuse settings is
                available for jobs started in this session.
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
