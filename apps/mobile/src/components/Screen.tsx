import React from 'react';
import {KeyboardAvoidingView, Platform, ScrollView, StyleSheet, View} from 'react-native';
import {SafeAreaView} from 'react-native-safe-area-context';

import {AppText} from './AppText';
import {useTheme} from '../theme';

export interface ScreenProps {
  title?: string;
  subtitle?: string;
  children: React.ReactNode;
  scroll?: boolean;
  footer?: React.ReactNode;
}

/**
 * Screen scaffold: safe-area padding, compact header, optional scrolling
 * content area and a pinned footer.
 */
export function Screen({
  title,
  subtitle,
  children,
  scroll = false,
  footer,
}: ScreenProps) {
  const theme = useTheme();
  return (
    <SafeAreaView
      style={[styles.safeArea, {backgroundColor: theme.colors.bg}]}
      edges={['top']}>
      <KeyboardAvoidingView
        style={styles.flex}
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
        keyboardVerticalOffset={0}>
        <View style={styles.flex}>
          {title !== undefined && (
            <View style={styles.header}>
              <AppText variant="title" numberOfLines={1}>
                {title}
              </AppText>
              {subtitle !== undefined && (
                <AppText variant="caption" tone="secondary">
                  {subtitle}
                </AppText>
              )}
            </View>
          )}
          {scroll ? (
            <ScrollView
              style={styles.flex}
              contentContainerStyle={styles.scrollContent}
              keyboardShouldPersistTaps="handled"
              showsVerticalScrollIndicator={false}>
              {children}
            </ScrollView>
          ) : (
            <View style={styles.flex}>{children}</View>
          )}
          {footer !== undefined && (
            <View
              style={[styles.footer, {borderColor: theme.colors.border}]}>
              {footer}
            </View>
          )}
        </View>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
  },
  flex: {
    flex: 1,
  },
  header: {
    paddingHorizontal: 16,
    paddingTop: 8,
    paddingBottom: 12,
  },
  scrollContent: {
    paddingBottom: 32,
  },
  footer: {
    borderTopWidth: 1,
    padding: 16,
  },
});
