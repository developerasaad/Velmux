import React, {useCallback} from 'react';
import {Alert, FlatList, RefreshControl, StyleSheet, View} from 'react-native';
import type {ListRenderItemInfo} from 'react-native';

import {AppText} from '../components/AppText';
import {Button} from '../components/Button';
import {EmptyState} from '../components/EmptyState';
import {ErrorState} from '../components/ErrorState';
import {JobRow} from '../components/JobRow';
import {Screen} from '../components/Screen';
import {canPauseJob, canResumeJob, isTerminalJob} from '../constants/jobStates';
import type {JobSnapshot} from '../native/NativeVelmux';
import {useJobsStore} from '../state/jobsStore';
import {useTheme} from '../theme';

export function QueueScreen() {
  const theme = useTheme();
  const jobs = useJobsStore(state => state.jobs);
  const requests = useJobsStore(state => state.requests);
  const initialized = useJobsStore(state => state.initialized);
  const refreshing = useJobsStore(state => state.refreshing);
  const refreshError = useJobsStore(state => state.refreshError);
  const refresh = useJobsStore(state => state.refresh);
  const cancelJob = useJobsStore(state => state.cancelJob);
  const pauseJob = useJobsStore(state => state.pauseJob);
  const resumeJob = useJobsStore(state => state.resumeJob);
  const retryJob = useJobsStore(state => state.retryJob);

  const confirmCancel = useCallback(
    (job: JobSnapshot) => {
      Alert.alert(
        'Cancel job',
        `Cancel the conversion of "${job.sourceName}"? Progress made so far will not be kept.`,
        [
          {text: 'Keep running', style: 'cancel'},
          {
            text: 'Cancel job',
            style: 'destructive',
            onPress: () => {
              cancelJob(job.id);
            },
          },
        ],
      );
    },
    [cancelJob],
  );

  const renderItem = useCallback(
    ({item}: ListRenderItemInfo<JobSnapshot>) => {
      const terminal = isTerminalJob(item.state);
      const hasRequest = requests[item.id] !== undefined;
      const showPause = canPauseJob(item.state);
      const showResume = canResumeJob(item.state);
      const showRetry = item.state === 'failed';
      const showCancel = !terminal;

      return (
        <JobRow
          job={item}
          actions={
            <View style={styles.actions}>
              {showCancel && (
                <Button
                  label="Cancel"
                  variant="secondary"
                  accessibilityLabel={`Cancel conversion of ${item.sourceName}`}
                  onPress={() => confirmCancel(item)}
                />
              )}
              {showPause && (
                <Button
                  label="Pause"
                  variant="secondary"
                  accessibilityLabel={`Pause ${item.sourceName}`}
                  onPress={() => {
                    pauseJob(item.id);
                  }}
                />
              )}
              {showResume && (
                <Button
                  label="Resume"
                  variant="secondary"
                  accessibilityLabel={`Resume ${item.sourceName}`}
                  onPress={() => {
                    resumeJob(item.id);
                  }}
                />
              )}
              {showRetry && (
                <View style={styles.retryBlock}>
                  <Button
                    label="Retry"
                    variant="secondary"
                    disabled={!hasRequest}
                    accessibilityLabel={`Retry ${item.sourceName}`}
                    onPress={() => {
                      retryJob(item.id);
                    }}
                  />
                  {!hasRequest && (
                    <AppText variant="micro" tone="muted">
                      Original request not available in this session
                    </AppText>
                  )}
                </View>
              )}
            </View>
          }
        />
      );
    },
    [confirmCancel, pauseJob, requests, resumeJob, retryJob],
  );

  const empty = jobs.length === 0;

  return (
    <Screen
      title="Queue"
      subtitle="Each job runs independently — one failed file never fails the rest.">
      {initialized && refreshError !== null && (
        <View style={styles.errorWrap}>
          <ErrorState
            title="Could not load jobs"
            message={refreshError}
            retryLabel="Retry"
            onRetry={() => {
              refresh();
            }}
          />
        </View>
      )}
      <FlatList
        data={jobs}
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
              title="Queue is empty"
              message="Start a compression or conversion from Home — jobs appear here with live progress."
            />
          </View>
        }
      />
    </Screen>
  );
}

const styles = StyleSheet.create({
  errorWrap: {
    paddingHorizontal: 16,
    paddingBottom: 12,
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
  actions: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 8,
  },
  retryBlock: {
    gap: 4,
  },
});
