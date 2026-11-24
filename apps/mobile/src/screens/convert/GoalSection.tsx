import React from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from '../../components/AppText';
import {OptionChips} from '../../components/OptionChips';
import {Section} from '../../components/Section';
import {TextField} from '../../components/TextField';
import {parseTargetSizeBytes} from '../../format/format';
import {computeGoalLabel, useConvertStore} from '../../state/convertStore';
import type {GoalKind, TargetSizeUnit} from '../../state/convertStore';

const GOAL_OPTIONS: ReadonlyArray<{label: string; value: GoalKind}> = [
  {label: 'Recommended', value: 'recommended'},
  {label: 'Smaller File', value: 'smallerFile'},
  {label: 'Fit Under Size', value: 'fitUnderSize'},
  {label: 'High Quality', value: 'highQuality'},
  {label: 'Custom', value: 'custom'},
];

const UNIT_OPTIONS: ReadonlyArray<{label: string; value: TargetSizeUnit}> = [
  {label: 'MB', value: 'MB'},
  {label: 'GB', value: 'GB'},
];

/** Goal selector; expresses intent, the engine derives parameters. */
export function GoalSection() {
  const draft = useConvertStore(state => state.draft);
  const setBasePreset = useConvertStore(state => state.setBasePreset);
  const setTargetSizeText = useConvertStore(state => state.setTargetSizeText);
  const setTargetSizeUnit = useConvertStore(state => state.setTargetSizeUnit);

  const {basePreset, targetSizeText, targetSizeUnit} = draft;
  const showTargetSize = basePreset === 'fitUnderSize';
  const targetBytes = parseTargetSizeBytes(targetSizeText, targetSizeUnit);

  return (
    <Section
      title="Goal"
      hint="The engine decides how to reach it — you never tune codecs to compress.">
      <OptionChips
        options={GOAL_OPTIONS}
        selected={basePreset}
        onSelect={setBasePreset}
        accessibilityLabelPrefix="Goal"
      />

      {showTargetSize && (
        <View style={styles.targetBlock}>
          <TextField
            label="Target size"
            value={targetSizeText}
            onChangeText={setTargetSizeText}
            placeholder="e.g. 25"
            keyboardType="decimal-pad"
            hint={
              targetBytes === null
                ? 'Enter the maximum output size for this file.'
                : undefined
            }
          />
          <OptionChips
            options={UNIT_OPTIONS}
            selected={targetSizeUnit}
            onSelect={setTargetSizeUnit}
            accessibilityLabelPrefix="Target size unit"
          />
        </View>
      )}

      <View
        style={styles.goalLabelRow}
        accessible
        accessibilityLabel={`Current goal, ${computeGoalLabel(draft)}`}>
        <AppText variant="caption" tone="secondary">
          Current goal
        </AppText>
        <AppText variant="captionStrong" tone="accent">
          {computeGoalLabel(draft)}
        </AppText>
      </View>
    </Section>
  );
}

const styles = StyleSheet.create({
  targetBlock: {
    gap: 12,
    marginBottom: 12,
  },
  goalLabelRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    minHeight: 32,
  },
});
