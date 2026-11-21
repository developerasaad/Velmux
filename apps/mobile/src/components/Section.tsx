import React from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';

export interface SectionProps {
  title: string;
  hint?: string;
  action?: React.ReactNode;
  children: React.ReactNode;
}

/** Titled group of controls. One consistent section rhythm across screens. */
export function Section({title, hint, action, children}: SectionProps) {
  const theme = useTheme();
  return (
    <View style={[styles.container, {borderColor: theme.colors.border}]}>
      <View style={styles.headerRow}>
        <View style={styles.headerText}>
          <AppText variant="title">{title}</AppText>
          {hint !== undefined && (
            <AppText variant="caption" tone="secondary">
              {hint}
            </AppText>
          )}
        </View>
        {action}
      </View>
      {children}
    </View>
  );
}

export interface RowProps {
  label: string;
  value?: string;
  children?: React.ReactNode;
}

/** A labeled field row used inside sections. */
export function FieldRow({label, value, children}: RowProps) {
  const theme = useTheme();
  return (
    <View
      style={[styles.row, {borderColor: theme.colors.border}]}
      accessibilityLabel={label}>
      <AppText variant="body" tone="secondary" style={styles.rowLabel}>
        {label}
      </AppText>
      {children !== undefined ? (
        children
      ) : (
        <AppText
          variant="body"
          tone="primary"
          numeric
          style={styles.rowValue}>
          {value ?? '—'}
        </AppText>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    paddingHorizontal: 16,
    paddingVertical: 16,
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  headerRow: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    justifyContent: 'space-between',
    gap: 12,
    marginBottom: 12,
  },
  headerText: {
    flex: 1,
    gap: 2,
  },
  row: {
    minHeight: 44,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
    borderBottomWidth: StyleSheet.hairlineWidth,
    paddingVertical: 8,
  },
  rowLabel: {
    flexShrink: 1,
  },
  rowValue: {
    textAlign: 'right',
  },
});
