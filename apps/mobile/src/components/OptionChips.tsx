import React from 'react';
import {StyleSheet, View, ViewStyle} from 'react-native';

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
        />
      ))}
    </View>
  );
}

interface OptionChipsStyles {
  wrap: ViewStyle;
}

const styles = StyleSheet.create<OptionChipsStyles>({
  wrap: {
    flexDirection: 'row',
    flexWrap: 'wrap',
  },
});
