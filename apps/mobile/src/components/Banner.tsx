import React from 'react';
import {Pressable, StyleSheet, View} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';

export interface BannerProps {
  message: string;
  tone?: 'warning' | 'info';
  onPress?: () => void;
}

/**
 * Slim persistent banner. Used for first-class conditions such as the
 * native engine not being linked in the current build.
 */
export function Banner({message, tone = 'warning', onPress}: BannerProps) {
  const theme = useTheme();
  const accentColor =
    tone === 'warning' ? theme.colors.warning : theme.colors.info;

  const content = (
    <View
      style={[
        styles.container,
        {borderColor: accentColor, backgroundColor: theme.colors.surface},
      ]}>
      <View style={[styles.mark, {backgroundColor: accentColor}]} />
      <AppText
        variant="caption"
        tone="primary"
        style={styles.message}
        numberOfLines={2}>
        {message}
      </AppText>
    </View>
  );

  if (onPress === undefined) {
    return content;
  }
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={message}
      onPress={onPress}
      style={({pressed}) => (pressed ? styles.pressed : null)}>
      {content}
    </Pressable>
  );
}

const styles = StyleSheet.create({
  container: {
    flexDirection: 'row',
    alignItems: 'center',
    borderTopWidth: 1,
    borderBottomWidth: 1,
    paddingHorizontal: 16,
    paddingVertical: 8,
    minHeight: 44,
    gap: 12,
  },
  mark: {
    width: 8,
    height: 8,
    borderRadius: 4,
  },
  message: {
    flex: 1,
  },
  pressed: {
    opacity: 0.7,
  },
});
