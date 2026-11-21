import React, {useCallback} from 'react';
import {Alert, Pressable, RefreshControl, ScrollView, StyleSheet, View} from 'react-native';
import {
  useNavigation,
  type CompositeScreenProps,
} from '@react-navigation/native';
import type {BottomTabScreenProps} from '@react-navigation/bottom-tabs';
import type {NativeStackScreenProps} from '@react-navigation/native-stack';
import {errorCodes, isErrorWithCode, pick, types} from '@react-native-documents/picker';

import {AppText} from '../components/AppText';
import {EmptyState} from '../components/EmptyState';
import {JobRow} from '../components/JobRow';
import {Screen} from '../components/Screen';
import {formatBytes, formatSavedPercent} from '../format/format';
import type {MediaSource} from '../native/NativeVelmux';
import {
  selectActiveJobs,
  selectCompletedJobs,
  useJobsStore,
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

export function HomeScreen() {
  const theme = useTheme();
  const navigation = useNavigation<HomeScreenProps['navigation']>();

  const activeJobs = useJobsStore(selectActiveJobs);
  const recentResults = useJobsStore(selectCompletedJobs);
  const refreshing = useJobsStore(state => state.refreshing);
  const refreshError = useJobsStore(state => state.refreshError);
  const refresh = useJobsStore(state => state.refresh);

  const pickAndRoute = useCallback(
    async (intent: PrimaryAction['id']) => {
      try {
        const [picked] = await pick({
          type: [types.video, types.audio],
          mode: 'open',
          // Durable, persistable source access: recovery must be able to
          // reopen the file long after the picker closed (spec: file access).
          requestLongTermAccess: true,
          allowMultiSelection: false,
        });
        const source: MediaSource = {
          uri: picked.uri,
          displayName: picked.name ?? 'Untitled media',
        };
        navigation.navigate('Convert', {source, intent});
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
    [navigation],
  );

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
            {activeJobs.map(job => (
              <JobRow key={job.id} job={job} compact />
            ))}
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
                ]}>
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
