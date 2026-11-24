import React, {useCallback, useEffect, useState} from 'react';
import {
  Alert,
  Pressable,
  RefreshControl,
  ScrollView,
  StyleSheet,
  View,
} from 'react-native';
import {
  useFocusEffect,
  useNavigation,
  type CompositeScreenProps,
} from '@react-navigation/native';
import type {BottomTabScreenProps} from '@react-navigation/bottom-tabs';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import {errorCodes, isErrorWithCode, pick, types} from '@react-native-documents/picker';

import {AppText} from '../components/AppText';
import {Banner} from '../components/Banner';
import {Button} from '../components/Button';
import {EmptyState} from '../components/EmptyState';
import {JobRow} from '../components/JobRow';
import {Screen} from '../components/Screen';
import {formatBytes, formatSavedPercent} from '../format/format';
import type {MediaSource} from '../native/NativeVelmux';
import {
  selectActiveJobs,
  selectCompletedJobs,
  useJobsStore,
  type BatchStartOutcome,
} from '../state/jobsStore';
import {useTheme} from '../theme';
import type {RootStackParamList, TabParamList} from '../app/types';

type HomeScreenProps = CompositeScreenProps<
  BottomTabScreenProps<TabParamList, 'Home'>,
  NativeStackScreenProps<RootStackParamList>
>;

interface PrimaryAction {
  id: 'compress' | 'convert' | 'audioExtract';
  title: string;
  caption: string;
}

const PRIMARY_ACTIONS: readonly PrimaryAction[] = [
  {
    id: 'compress',
    title: 'Compress Video',
    caption: 'Smaller file, balanced quality',
  },
  {
    id: 'convert',
    title: 'Convert Video',
    caption: 'Change format or quality',
  },
  {
    id: 'audioExtract',
    title: 'Extract Audio',
    caption: 'Save the sound track',
  },
];

/** Home shows a bounded slice of active jobs; Queue owns the full list. */
const ACTIVE_JOBS_SHOWN = 6;

/** Bounded height for the multi-select file list (long-list rule). */
const SELECTION_LIST_MAX_HEIGHT = 216;

/** One row of the staged multi-file selection. */
interface PickedFile {
  uri: string;
  name: string;
  sizeBytes: number | null;
}

/** A multi-file selection awaiting the "Convert N files" confirmation. */
interface FileSelection {
  intent: PrimaryAction['id'];
  files: PickedFile[];
}

function batchCtaLabel(intent: PrimaryAction['id'], count: number): string {
  const files = `${count} ${count === 1 ? 'file' : 'files'}`;
  switch (intent) {
    case 'compress':
      return `Compress ${files}`;
    case 'convert':
      return `Convert ${files}`;
    case 'audioExtract':
      return `Extract audio from ${files}`;
  }
}

export function HomeScreen() {
  const theme = useTheme();
  const navigation = useNavigation<HomeScreenProps['navigation']>();

  const activeJobs = useJobsStore(selectActiveJobs);
  const recentResults = useJobsStore(selectCompletedJobs);
  const refreshing = useJobsStore(state => state.refreshing);
  const refreshError = useJobsStore(state => state.refreshError);
  const refresh = useJobsStore(state => state.refresh);
  const beginBatch = useJobsStore(state => state.beginBatch);
  const clearBatch = useJobsStore(state => state.clearBatch);

  const [selection, setSelection] = useState<FileSelection | null>(null);
  const [batchOutcome, setBatchOutcome] = useState<BatchStartOutcome | null>(
    null,
  );

  // Batch feedback is produced while the conversion workspace is on top;
  // consume it once when Home regains focus.
  useFocusEffect(
    useCallback(() => {
      const outcome = useJobsStore.getState().consumeBatchStart();
      if (outcome !== null) {
        setBatchOutcome(outcome);
      }
    }, []),
  );

  // Honest partial-failure reporting: name every file that could not start.
  useEffect(() => {
    if (batchOutcome === null || batchOutcome.failures.length === 0) {
      return;
    }
    const failureLines = batchOutcome.failures
      .map(failure => `• ${failure.name} — ${failure.reason}`)
      .join('\n');
    Alert.alert(
      'Batch partially started',
      `${batchOutcome.started} of ${batchOutcome.total} files were queued.\n\nCould not start:\n${failureLines}`,
      [
        {
          text: 'View queue',
          onPress: () => {
            setBatchOutcome(null);
            navigation.navigate('Queue');
          },
        },
        {
          text: 'Close',
          style: 'cancel',
          onPress: () => {
            setBatchOutcome(null);
          },
        },
      ],
    );
  }, [batchOutcome, navigation]);

  const pickAndRoute = useCallback(
    async (intent: PrimaryAction['id']) => {
      try {
        const picked = await pick({
          type: [types.video, types.audio],
          mode: 'open',
          // Durable, persistable source access: recovery must be able to
          // reopen the file long after the picker closed (spec: file access).
          requestLongTermAccess: true,
          allowMultiSelection: true,
        });
        if (picked.length === 1) {
          // Single-source flow: straight into the workspace, no batch.
          const [only] = picked;
          const source: MediaSource = {
            uri: only.uri,
            displayName: only.name ?? 'Untitled media',
          };
          clearBatch();
          navigation.navigate('Convert', {source, intent});
          return;
        }
        setSelection({
          intent,
          files: picked.map(file => ({
            uri: file.uri,
            name: file.name ?? 'Untitled media',
            sizeBytes: file.size,
          })),
        });
      } catch (error) {
        if (
          isErrorWithCode(error) &&
          error.code === errorCodes.OPERATION_CANCELED
        ) {
          return;
        }
        Alert.alert(
          'Could not open picker',
          error instanceof Error ? error.message : 'Unknown error',
        );
      }
    },
    [clearBatch, navigation],
  );

  const removePicked = useCallback((uri: string) => {
    setSelection(current => {
      if (current === null) {
        return null;
      }
      const files = current.files.filter(file => file.uri !== uri);
      return files.length > 0 ? {intent: current.intent, files} : null;
    });
  }, []);

  const continueBatch = useCallback(() => {
    if (selection === null) {
      return;
    }
    const sources: MediaSource[] = selection.files.map(file => ({
      uri: file.uri,
      displayName: file.name,
    }));
    const [primary, ...extras] = sources;
    // The workspace opens with the first file; the rest are started as
    // their own jobs with the same settings when Start is pressed.
    beginBatch(primary, extras);
    setSelection(null);
    navigation.navigate('Convert', {source: primary, intent: selection.intent});
  }, [beginBatch, navigation, selection]);

  const selectionSummary = (() => {
    if (selection === null) {
      return null;
    }
    const count = selection.files.length;
    const countLabel = `${count} ${count === 1 ? 'file' : 'files'} selected`;
    const allSizesKnown = selection.files.every(
      file => file.sizeBytes !== null,
    );
    if (!allSizesKnown) {
      return countLabel;
    }
    const totalBytes = selection.files.reduce(
      (sum, file) => sum + (file.sizeBytes ?? 0),
      0,
    );
    return `${countLabel} · ${formatBytes(totalBytes)}`;
  })();

  return (
    <Screen>
      <ScrollView
        style={styles.flex}
        contentContainerStyle={styles.content}
        showsVerticalScrollIndicator={false}
        refreshControl={
          <RefreshControl
            refreshing={refreshing}
            onRefresh={() => {
              refresh();
            }}
            tintColor={theme.colors.textSecondary}
            colors={[theme.colors.accent]}
          />
        }>
        <View style={styles.wordmarkRow}>
          <AppText variant="titleLarge">Velmux</AppText>
          <AppText variant="caption" tone="muted">
            Convert · Compress · Audio
          </AppText>
        </View>

        <View style={styles.actionGrid}>
          {PRIMARY_ACTIONS.map(action => (
            <Pressable
              key={action.id}
              accessibilityRole="button"
              accessibilityLabel={action.title}
              onPress={() => {
                pickAndRoute(action.id);
              }}
              style={({pressed}) => [
                styles.actionTile,
                {backgroundColor: theme.colors.surface},
                pressed && styles.pressed,
              ]}>
              <AppText variant="captionStrong" numberOfLines={2}>
                {action.title}
              </AppText>
              <AppText variant="micro" tone="muted" numberOfLines={2}>
                {action.caption}
              </AppText>
            </Pressable>
          ))}
        </View>

        {selection !== null && (
          <View
            style={[
              styles.selectionCard,
              {backgroundColor: theme.colors.surface},
            ]}>
            <View style={styles.selectionHeader}>
              <AppText variant="captionStrong">Selected files</AppText>
              <AppText variant="caption" tone="secondary" numeric>
                {selectionSummary}
              </AppText>
            </View>
            <ScrollView
              style={styles.selectionList}
              contentContainerStyle={styles.selectionListContent}
              showsVerticalScrollIndicator={false}>
              {selection.files.map(file => (
                <View key={file.uri} style={styles.pickedRow}>
                  <AppText
                    variant="caption"
                    numberOfLines={1}
                    style={styles.pickedName}>
                    {file.name}
                  </AppText>
                  <AppText variant="caption" tone="secondary" numeric>
                    {formatBytes(file.sizeBytes)}
                  </AppText>
                  <Pressable
                    accessibilityRole="button"
                    accessibilityLabel={`Remove ${file.name}`}
                    hitSlop={10}
                    onPress={() => {
                      removePicked(file.uri);
                    }}
                    style={({pressed}) => [
                      styles.removeButton,
                      pressed && styles.pressed,
                    ]}>
                    <AppText variant="captionStrong" tone="danger">
                      ✕
                    </AppText>
                  </Pressable>
                </View>
              ))}
            </ScrollView>
            <Button
              label={batchCtaLabel(selection.intent, selection.files.length)}
              variant="primary"
              onPress={() => {
                continueBatch();
              }}
            />
            {selection.files.length > 1 && (
              <AppText variant="micro" tone="muted" style={styles.batchNote}>
                The settings you choose next apply to all{' '}
                {selection.files.length} files.
              </AppText>
            )}
            <Button
              label="Clear selection"
              variant="ghost"
              onPress={() => {
                setSelection(null);
              }}
            />
          </View>
        )}

        {batchOutcome !== null && batchOutcome.failures.length === 0 && (
          <Banner
            message={`Started ${batchOutcome.started} conversions — tap to view the queue.`}
            tone="info"
            onPress={() => {
              setBatchOutcome(null);
              navigation.navigate('Queue');
            }}
          />
        )}

        <View style={styles.sectionHeader}>
          <AppText variant="title">Active jobs</AppText>
        </View>
        {refreshError !== null && (
          <AppText variant="caption" tone="danger" style={styles.block}>
            {refreshError}
          </AppText>
        )}
        {activeJobs.length === 0 ? (
          <EmptyState
            title="No active jobs"
            message="Select a video to compress or convert — running jobs appear here."
          />
        ) : (
          <View style={styles.stack}>
            {activeJobs.slice(0, ACTIVE_JOBS_SHOWN).map(job => (
              <JobRow key={job.id} job={job} compact />
            ))}
            {activeJobs.length > ACTIVE_JOBS_SHOWN && (
              <Pressable
                accessibilityRole="button"
                accessibilityLabel={`See all ${activeJobs.length} jobs in Queue`}
                onPress={() => navigation.navigate('Queue')}
                style={({pressed}) => [styles.seeAll, pressed && styles.pressed]}>
                <AppText variant="captionStrong" tone="accent">
                  See all {activeJobs.length} in Queue
                </AppText>
              </Pressable>
            )}
          </View>
        )}

        <View style={styles.sectionHeader}>
          <AppText variant="title">Recent results</AppText>
          <Pressable
            accessibilityRole="button"
            accessibilityLabel="See all in History"
            onPress={() => navigation.navigate('History')}
            hitSlop={8}
            style={({pressed}) => [styles.seeAll, pressed && styles.pressed]}>
            <AppText variant="captionStrong" tone="accent">
              See all
            </AppText>
          </Pressable>
        </View>
        {recentResults.length === 0 ? (
          <EmptyState
            title="No results yet"
            message="Completed conversions appear here with space saved."
          />
        ) : (
          <View style={styles.stack}>
            {recentResults.slice(0, 5).map(job => (
              <View
                key={job.id}
                style={[
                  styles.resultRow,
                  {backgroundColor: theme.colors.surface},
                ]}
                accessible
                accessibilityLabel={[
                  job.sourceName,
                  `${formatBytes(job.inputSizeBytes)} to ${formatBytes(
                    job.outputSizeBytes,
                  )}`,
                  `Saved ${formatSavedPercent(
                    job.inputSizeBytes,
                    job.outputSizeBytes,
                  )}`,
                ].join(', ')}>
                <AppText
                  variant="captionStrong"
                  numberOfLines={1}
                  style={styles.resultName}>
                  {job.sourceName}
                </AppText>
                <AppText variant="caption" tone="secondary" numeric>
                  {formatBytes(job.inputSizeBytes)} →{' '}
                  {formatBytes(job.outputSizeBytes)}
                </AppText>
                <AppText variant="caption" tone="success" numeric>
                  Saved{' '}
                  {formatSavedPercent(job.inputSizeBytes, job.outputSizeBytes)}
                </AppText>
              </View>
            ))}
          </View>
        )}
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  flex: {
    flex: 1,
  },
  content: {
    padding: 16,
    gap: 16,
    paddingBottom: 32,
  },
  wordmarkRow: {
    gap: 2,
  },
  actionGrid: {
    flexDirection: 'row',
    gap: 8,
  },
  actionTile: {
    flex: 1,
    minHeight: 84,
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    padding: 10,
    gap: 4,
    justifyContent: 'center',
  },
  selectionCard: {
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    padding: 12,
    gap: 10,
  },
  selectionHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
  selectionList: {
    maxHeight: SELECTION_LIST_MAX_HEIGHT,
  },
  selectionListContent: {
    gap: 2,
  },
  pickedRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    minHeight: 36,
  },
  pickedName: {
    flexShrink: 1,
  },
  removeButton: {
    minWidth: 36,
    minHeight: 36,
    alignItems: 'center',
    justifyContent: 'center',
  },
  batchNote: {
    textAlign: 'center',
  },
  sectionHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginTop: 8,
  },
  stack: {
    gap: 8,
  },
  block: {
    marginBottom: 4,
  },
  resultRow: {
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    padding: 12,
    gap: 4,
  },
  resultName: {
    flexShrink: 1,
  },
  seeAll: {
    minHeight: 44,
    justifyContent: 'center',
  },
  pressed: {
    opacity: 0.7,
  },
});
