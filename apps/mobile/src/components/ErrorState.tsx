import React from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from './AppText';
import {Button} from './Button';
import {useTheme} from '../theme';

export interface ErrorStateProps {
  title?: string;
  message: string;
  retryLabel?: string;
  onRetry?: () => void;
}

/** Honest error state; offers retry when the operation can be repeated. */
export function ErrorState({
  title = 'Something went wrong',
  message,
  retryLabel,
  onRetry,
}: ErrorStateProps) {
  const theme = useTheme();
  return (
    <View
      style={[styles.container, {borderColor: theme.colors.danger}]}>
      <AppText variant="bodyStrong" tone="danger">
        {title}
      </AppText>
      <AppText variant="caption" tone="secondary" style={styles.message}>
        {message}
      </AppText>
      {onRetry !== undefined && retryLabel !== undefined && (
        <View style={styles.retry}>
          <Button label={retryLabel} variant="secondary" onPress={onRetry} />
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    borderRadius: 12,
    borderWidth: 1,
    padding: 16,
    gap: 4,
  },
  message: {
    textAlign: 'left',
  },
  retry: {
    alignSelf: 'flex-start',
    marginTop: 8,
  },
});
