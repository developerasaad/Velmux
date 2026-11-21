import React from 'react';
import {
  ActivityIndicator,
  Pressable,
  StyleSheet,
  ViewStyle,
} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';
import type {AppTextTone} from './AppText';

export type ButtonVariant = 'primary' | 'secondary' | 'ghost' | 'dangerGhost';

export interface ButtonProps {
  label: string;
  onPress: () => void;
  variant?: ButtonVariant;
  disabled?: boolean;
  loading?: boolean;
  size?: 'regular' | 'large';
  testID?: string;
}

export function Button({
  label,
  onPress,
  variant = 'primary',
  disabled = false,
  loading = false,
  size = 'regular',
  testID,
}: ButtonProps) {
  const theme = useTheme();
  const isPrimary = variant === 'primary';
  const isFlat = variant === 'ghost' || variant === 'dangerGhost';
  const inactive = disabled || loading;

  const restingBackground = (() => {
    switch (variant) {
      case 'primary':
        return theme.colors.accent;
      case 'secondary':
        return theme.colors.surface;
      case 'ghost':
      case 'dangerGhost':
        return 'transparent';
    }
  })();

  const pressedBackground = (() => {
    switch (variant) {
      case 'primary':
        return theme.colors.accentPressed;
      case 'secondary':
        return theme.colors.elevated;
      case 'ghost':
      case 'dangerGhost':
        return 'transparent';
    }
  })();

  const labelTone: AppTextTone = (() => {
    switch (variant) {
      case 'primary':
        return 'onAccent';
      case 'secondary':
        return 'primary';
      case 'ghost':
        return 'accent';
      case 'dangerGhost':
        return 'danger';
    }
  })();

  const height = size === 'large' ? 48 : 44;

  return (
    <Pressable
      testID={testID}
      accessibilityRole="button"
      accessibilityLabel={label}
      accessibilityState={{disabled: inactive}}
      disabled={inactive}
      onPress={onPress}
      style={({pressed}) => [
        styles.base,
        {height, backgroundColor: restingBackground},
        variant === 'secondary' && {borderColor: theme.colors.border},
        variant === 'secondary' && styles.bordered,
        inactive && styles.inactive,
        pressed &&
          !inactive &&
          (isFlat
            ? styles.flatPressed
            : {backgroundColor: pressedBackground}),
      ]}>
      {loading ? (
        <ActivityIndicator
          color={isPrimary ? theme.colors.onAccent : theme.colors.accent}
          size="small"
        />
      ) : (
        <AppText variant="bodyStrong" tone={labelTone}>
          {label}
        </AppText>
      )}
    </Pressable>
  );
}

interface ButtonStyles {
  base: ViewStyle;
  bordered: ViewStyle;
  inactive: ViewStyle;
  flatPressed: ViewStyle;
}

const styles = StyleSheet.create<ButtonStyles>({
  base: {
    minHeight: 44,
    paddingHorizontal: 16,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    flexDirection: 'row',
  },
  bordered: {
    borderWidth: 1,
  },
  inactive: {
    opacity: 0.45,
  },
  flatPressed: {
    opacity: 0.65,
  },
});
