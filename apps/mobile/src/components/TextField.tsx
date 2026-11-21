import React from 'react';
import {StyleSheet, TextInput, View} from 'react-native';

import {AppText} from './AppText';
import {useTheme} from '../theme';

export interface TextFieldProps {
  label: string;
  value: string;
  onChangeText: (text: string) => void;
  placeholder?: string;
  keyboardType?: 'default' | 'decimal-pad' | 'number-pad';
  suffix?: string;
  hint?: string;
  testID?: string;
}

/** Labeled single-line input used for target sizes and custom values. */
export function TextField({
  label,
  value,
  onChangeText,
  placeholder,
  keyboardType = 'default',
  suffix,
  hint,
  testID,
}: TextFieldProps) {
  const theme = useTheme();
  return (
    <View style={styles.container}>
      <AppText variant="caption" tone="secondary">
        {label}
      </AppText>
      <View style={styles.inputRow}>
        <View
          style={[
            styles.inputWrap,
            {borderColor: theme.colors.border, backgroundColor: theme.colors.bg},
          ]}>
          <TextInput
            testID={testID}
            accessibilityLabel={label}
            value={value}
            onChangeText={onChangeText}
            placeholder={placeholder}
            placeholderTextColor={theme.colors.textMuted}
            keyboardType={keyboardType}
            style={[styles.input, {color: theme.colors.textPrimary}]}
          />
          {suffix !== undefined && (
            <AppText variant="body" tone="muted" numeric>
              {suffix}
            </AppText>
          )}
        </View>
      </View>
      {hint !== undefined && (
        <AppText variant="micro" tone="muted">
          {hint}
        </AppText>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    gap: 6,
  },
  inputRow: {
    minHeight: 44,
  },
  inputWrap: {
    flexDirection: 'row',
    alignItems: 'center',
    borderWidth: 1,
    borderRadius: 12,
    paddingHorizontal: 12,
    minHeight: 44,
  },
  input: {
    flex: 1,
    paddingVertical: 10,
    fontSize: 15,
    fontVariant: ['tabular-nums'],
  },
});
