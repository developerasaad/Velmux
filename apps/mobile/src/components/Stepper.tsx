import React from 'react';
import {Pressable, StyleSheet, View} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';
import {minTouchTarget} from '../theme';

export interface StepperProps {
  value: number | null;
  onChange: (value: number | null) => void;
  step: number;
  min?: number;
  max?: number;
  /** Shown when the value is null ("Auto" / "Original" semantics). */
  nullLabel?: string;
  /** Value set when stepping away from null. */
  initialWhenNull?: number;
  format?: (value: number) => string;
  testID?: string;
  /** Control name for screen readers, e.g. "frame rate" — announces
   * "Increase frame rate, 30 fps" / "Decrease frame rate, 30 fps". */
  accessibilityLabel?: string;
}

/**
 * Numeric stepper for exact advanced values. A null value means "engine
 * decides" and is always reachable via the minus/reset button.
 */
export function Stepper({
  value,
  onChange,
  step,
  min,
  max,
  nullLabel = 'Auto',
  initialWhenNull,
  format,
  testID,
  accessibilityLabel,
}: StepperProps) {
  const theme = useTheme();
  const display =
    value === null
      ? nullLabel
      : format
        ? format(value)
        : String(Math.round(value * 100) / 100);

  const decreaseLabel = accessibilityLabel
    ? `Decrease ${accessibilityLabel}, ${display}`
    : 'Decrease value';
  const increaseLabel = accessibilityLabel
    ? `Increase ${accessibilityLabel}, ${display}`
    : 'Increase value';

  const stepValue = (direction: 1 | -1) => {
    if (value === null) {
      const start =
        initialWhenNull !== undefined ? initialWhenNull : (min ?? step);
      onChange(clamp(start, min, max));
      return;
    }
    if (direction === -1) {
      const next = value - step;
      if (min !== undefined && next < min) {
        onChange(min);
        return;
      }
      onChange(round(next));
      return;
    }
    const next = value + step;
    onChange(clamp(round(next), min, max));
  };

  const minusDisabled = value !== null && min !== undefined && value <= min;
  const plusDisabled = value !== null && max !== undefined && value >= max;

  return (
    <View
      style={[styles.container, {borderColor: theme.colors.border}]}
      testID={testID}>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={decreaseLabel}
        accessibilityState={{disabled: minusDisabled}}
        disabled={minusDisabled}
        onPress={() => stepValue(-1)}
        style={({pressed}) => [
          styles.button,
          styles.buttonMinus,
          {borderColor: theme.colors.border},
          minusDisabled && styles.disabled,
          pressed && !minusDisabled && styles.pressed,
        ]}>
        <AppText variant="bodyStrong" tone="primary">
          −
        </AppText>
      </Pressable>
      <AppText
        variant="body"
        numeric
        tone="primary"
        style={styles.value}
        numberOfLines={1}>
        {display}
      </AppText>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={increaseLabel}
        accessibilityState={{disabled: plusDisabled}}
        disabled={plusDisabled}
        onPress={() => stepValue(1)}
        style={({pressed}) => [
          styles.button,
          styles.buttonPlus,
          {borderColor: theme.colors.border},
          plusDisabled && styles.disabled,
          pressed && styles.pressed,
        ]}>
        <AppText variant="bodyStrong" tone="primary">
          +
        </AppText>
      </Pressable>
    </View>
  );
}

function clamp(value: number, min?: number, max?: number): number {
  let result = value;
  if (min !== undefined && result < min) {
    result = min;
  }
  if (max !== undefined && result > max) {
    result = max;
  }
  return round(result);
}

function round(value: number): number {
  return Math.round(value * 1000) / 1000;
}

const styles = StyleSheet.create({
  container: {
    flexDirection: 'row',
    alignItems: 'center',
    borderWidth: 1,
    borderRadius: 12,
    overflow: 'hidden',
  },
  button: {
    width: minTouchTarget,
    height: minTouchTarget,
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonMinus: {
    borderRightWidth: StyleSheet.hairlineWidth,
  },
  buttonPlus: {
    borderLeftWidth: StyleSheet.hairlineWidth,
  },
  value: {
    minWidth: 72,
    textAlign: 'center',
    paddingHorizontal: 8,
  },
  disabled: {
    opacity: 0.35,
  },
  pressed: {
    opacity: 0.6,
  },
});
