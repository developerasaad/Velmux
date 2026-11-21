import React, {useCallback} from 'react';
import {Alert, StyleSheet, View} from 'react-native';
import {errorCodes, isErrorWithCode, pick, types} from '@react-native-documents/picker';

import {AppText} from '../../components/AppText';
import {Button} from '../../components/Button';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {Stepper} from '../../components/Stepper';
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
      setExternalSubtitle({
        uri: picked.uri,
        name: picked.name ?? 'Subtitle file',
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
