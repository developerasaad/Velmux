import React from 'react';
import {Text, TextProps} from 'react-native';

import {useTheme} from '../theme';
import type {TypeVariant} from '../theme';
import {numericFontVariant} from '../theme';

export type AppTextTone =
  | 'primary'
  | 'secondary'
  | 'muted'
  | 'accent'
  | 'success'
  | 'warning'
  | 'danger'
  | 'info'
  | 'onAccent';

export interface AppTextProps extends TextProps {
  variant?: TypeVariant;
  tone?: AppTextTone;
  /** Renders sizes, rates and percentages with tabular figures. */
  numeric?: boolean;
}

function toneColor(
  tone: AppTextTone,
  colors: ReturnType<typeof useTheme>['colors'],
): string {
  switch (tone) {
    case 'primary':
      return colors.textPrimary;
    case 'secondary':
      return colors.textSecondary;
    case 'muted':
      return colors.textMuted;
    case 'accent':
      return colors.accent;
    case 'success':
      return colors.success;
    case 'warning':
      return colors.warning;
    case 'danger':
      return colors.danger;
    case 'info':
      return colors.info;
    case 'onAccent':
      return colors.onAccent;
  }
}

export function AppText({
  variant = 'body',
  tone = 'primary',
  numeric = false,
  style,
  ...rest
}: AppTextProps) {
  const theme = useTheme();
  return (
    <Text
      {...rest}
      style={[
        {color: toneColor(tone, theme.colors)},
        theme.type[variant],
        numeric ? {fontVariant: numericFontVariant} : null,
        style,
      ]}
    />
  );
}
