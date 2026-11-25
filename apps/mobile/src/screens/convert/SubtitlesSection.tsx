import React, {useCallback} from 'react';
import {Alert, StyleSheet, View} from 'react-native';
import {errorCodes, isErrorWithCode, pick, types} from '@react-native-documents/picker';

import {AppText} from '../../components/AppText';
import {Button} from '../../components/Button';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {Stepper} from '../../components/Stepper';
import {importPickedFile} from '../../native/fileAccess';
import {useConvertStore} from '../../state/convertStore';
import type {SubtitlePolicy} from '../../state/convertStore';

const POLICY_OPTIONS: ReadonlyArray<{label: string; value: SubtitlePolicy}> = [
  {label: 'Preserve', value: 'preserve'},
  {label: 'Remove', value: 'remove'},
  {label: 'Extract', value: 'extract'},
  {label: 'Burn in', value: 'burnIn'},
];

/**
 * Subtitle handling. Subtitles are never silently discarded during
 * ordinary conversion — removing them is an explicit choice here.
 */
export function SubtitlesSection() {
  const draft = useConvertStore(state => state.draft);
  const setSubtitlePolicy = useConvertStore(state => state.setSubtitlePolicy);
  const setExternalSubtitle = useConvertStore(
    state => state.setExternalSubtitle,
  );
  const setTimingOffsetMs = useConvertStore(state => state.setTimingOffsetMs);

  const pickExternalSubtitle = useCallback(async () => {
    try {
      const [picked] = await pick({
        type: [types.allFiles],
        mode: 'open',
        requestLongTermAccess: true,
        allowMultiSelection: false,
      });
      if (picked == null) {
        return;
      }
      // Durable local copy: the engine consumes external subtitle files
      // through its local-file contract, so the pick is imported into
      // app-durable storage once (content:// URIs and scoped in-place
      // URLs are not readable by the backends).
      const {source, failure} = await importPickedFile(picked);
      if (source == null) {
        Alert.alert(
          'Could not import subtitle',
          failure ? `${failure.name}: ${failure.reason}` : 'the copy failed',
        );
        return;
      }
      setExternalSubtitle({
        uri: source.uri,
        name: source.displayName,
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
  }, [setExternalSubtitle]);

  return (
    <Section
      title="Subtitles"
      hint="Timing offset applies to burned-in and extracted subtitles.">
      <OptionChips
        options={POLICY_OPTIONS}
        selected={draft.subtitles.policy}
        onSelect={setSubtitlePolicy}
        accessibilityLabelPrefix="Subtitle behavior"
      />

      <View style={styles.block}>
        <AppText variant="caption" tone="secondary">
          Timing offset
        </AppText>
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

      <View style={styles.block}>
        <AppText variant="caption" tone="secondary">
          External subtitle file
        </AppText>
        {draft.subtitles.externalUri === null ? (
          <Button
            label="Add subtitle file"
            variant="secondary"
            onPress={() => {
              pickExternalSubtitle();
            }}
          />
        ) : (
          <View style={styles.externalRow}>
            <AppText
              variant="caption"
              tone="primary"
              numberOfLines={1}
              style={styles.externalName}>
              {draft.subtitles.externalName ?? 'Selected subtitle'}
            </AppText>
            <Button
              label="Remove"
              variant="ghost"
              accessibilityLabel="Remove subtitle file"
              onPress={() => setExternalSubtitle(null)}
            />
          </View>
        )}
      </View>
    </Section>
  );
}

const styles = StyleSheet.create({
  block: {
    gap: 8,
    marginTop: 4,
  },
  externalRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  externalName: {
    flexShrink: 1,
  },
});
