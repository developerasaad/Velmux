import React from 'react';
import {StyleSheet, View} from 'react-native';

import {Chip} from './Chip';

export interface OptionItem<T> {
  label: string;
  value: T;
}

export interface OptionChipsProps<T> {
  options: ReadonlyArray<OptionItem<T>>;
  selected: T;
  onSelect: (value: T) => void;
  disabled?: boolean;
  testID?: string;
  /** Composed into each chip's screen-reader label so options are heard
   * with their group context (e.g. "Video codec, H.264"). */
  accessibilityLabelPrefix?: string;
}

/**
 * Wrapped row of selectable chips. Equality is by value for primitives and
 * by reference for objects (callers pass stable option lists).
 */
export function OptionChips<T>({
  options,
  selected,
  onSelect,
  disabled = false,
  testID,
  accessibilityLabelPrefix,
}: OptionChipsProps<T>) {
  return (
    <View style={styles.wrap} testID={testID}>
      {options.map(option => (
        <Chip
          key={option.label}
          label={option.label}
          selected={option.value === selected}
          disabled={disabled}
          onPress={() => onSelect(option.value)}
          accessibilityLabel={
            accessibilityLabelPrefix !== undefined
              ? `${accessibilityLabelPrefix}, ${option.label}`
              : undefined
          }
        />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: {
    flexDirection: 'row',
    flexWrap: 'wrap',
  },
});
