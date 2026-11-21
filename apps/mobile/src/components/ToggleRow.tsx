import React from 'react';
import {StyleSheet, Switch, View} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';

export interface ToggleRowProps {
  label: string;
  value: boolean;
  onValueChange: (value: boolean) => void;
  disabled?: boolean;
}

/** Settings toggle row with a 44 pt touch area. */
export function ToggleRow({
  label,
  value,
  onValueChange,
  disabled = false,
}: ToggleRowProps) {
  const theme = useTheme();
  return (
    <View style={styles.row}>
      <AppText
        variant="body"
        tone={disabled ? 'muted' : 'primary'}
        style={styles.label}>
        {label}
      </AppText>
      <Switch
        accessibilityLabel={label}
        value={value}
        disabled={disabled}
        onValueChange={onValueChange}
        trackColor={{
          true: theme.colors.accent,
          false: theme.colors.border,
        }}
        thumbColor={theme.colors.surface}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  row: {
    minHeight: 44,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 16,
    paddingVertical: 4,
  },
  label: {
    flex: 1,
  },
});
