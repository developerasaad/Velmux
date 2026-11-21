import React from 'react';
import {StyleSheet, TextStyle, View, ViewStyle} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';

export interface EmptyStateProps {
  title: string;
  message?: string;
}

/** Empty state that teaches the next action (design system rule). */
export function EmptyState({title, message}: EmptyStateProps) {
  const theme = useTheme();
  return (
    <View
      style={[styles.container, {backgroundColor: theme.colors.surface}]}>
      <AppText variant="bodyStrong" tone="secondary">
        {title}
      </AppText>
      {message !== undefined && (
        <AppText variant="caption" tone="muted" style={styles.message}>
          {message}
        </AppText>
      )}
    </View>
  );
}

interface EmptyStateStyles {
  container: ViewStyle;
  message: TextStyle;
}

const styles = StyleSheet.create<EmptyStateStyles>({
  container: {
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    borderStyle: 'dashed',
    padding: 20,
    alignItems: 'center',
    gap: 4,
  },
  message: {
    textAlign: 'center',
  },
});
