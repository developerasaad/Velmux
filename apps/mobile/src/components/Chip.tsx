import React from 'react';
import {Pressable, StyleSheet} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';
import type {AppTextTone} from './AppText';
import type {JobStateTone} from '../constants/jobStates';

export interface ChipProps {
  label: string;
  selected: boolean;
  onPress: () => void;
  disabled?: boolean;
  testID?: string;
}

/** Selectable option chip. Touch target is at least 44 pt. */
export function Chip({
  label,
  selected,
  onPress,
  disabled = false,
  testID,
}: ChipProps) {
  const theme = useTheme();
  return (
    <Pressable
      testID={testID}
      accessibilityRole="button"
      accessibilityLabel={label}
      accessibilityState={{selected, disabled}}
      disabled={disabled}
      onPress={onPress}
      style={({pressed}) => [
        styles.chip,
        {
          backgroundColor: selected
            ? theme.colors.accent
            : theme.colors.surface,
          borderColor: selected
            ? theme.colors.accent
            : theme.colors.border,
        },
        disabled && styles.disabled,
        pressed && !disabled && styles.pressed,
      ]}>
      <AppText
        variant="captionStrong"
        tone={selected ? 'onAccent' : 'primary'}
        numberOfLines={1}>
        {label}
      </AppText>
    </Pressable>
  );
}

export interface StateChipProps {
  label: string;
  tone: JobStateTone;
}

/** Non-interactive state indicator used on job rows. */
export function StateChip({label, tone}: StateChipProps) {
  const theme = useTheme();
  const textTone: AppTextTone =
    tone === 'muted' ? 'secondary' : (tone as AppTextTone);
  const borderColor = (() => {
    switch (tone) {
      case 'muted':
        return theme.colors.border;
      case 'info':
        return theme.colors.info;
      case 'accent':
        return theme.colors.accent;
      case 'warning':
        return theme.colors.warning;
      case 'success':
        return theme.colors.success;
      case 'danger':
        return theme.colors.danger;
    }
  })();
  return (
    <AppText
      variant="micro"
      tone={textTone}
      style={[styles.stateChip, {borderColor}]}
      numberOfLines={1}>
      {label}
    </AppText>
  );
}

const styles = StyleSheet.create({
  chip: {
    minHeight: 44,
    paddingHorizontal: 14,
    paddingTop: 4,
    paddingBottom: 4,
    borderRadius: 999,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
    marginRight: 8,
    marginBottom: 8,
  },
  disabled: {
    opacity: 0.4,
  },
  pressed: {
    opacity: 0.7,
  },
  stateChip: {
    borderWidth: 1,
    borderRadius: 999,
    paddingHorizontal: 8,
    paddingVertical: 2,
    overflow: 'hidden',
  },
});
