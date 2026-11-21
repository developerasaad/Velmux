import React, {createContext, useContext, useMemo} from 'react';
import {useColorScheme} from 'react-native';

import {useSettingsStore} from '../state/settingsStore';
import type {ThemeMode} from '../state/settingsStore';
import {darkPalette, lightPalette, radii, spacing, type} from './tokens';
import type {Palette} from './tokens';

export type {ThemeMode};
export type ResolvedThemeMode = 'dark' | 'light';

export interface VelmuxTheme {
  mode: ResolvedThemeMode;
  colors: Palette;
  spacing: typeof spacing;
  radii: typeof radii;
  type: typeof type;
}

function buildTheme(mode: ResolvedThemeMode): VelmuxTheme {
  return {
    mode,
    colors: mode === 'dark' ? darkPalette : lightPalette,
    spacing,
    radii,
    type,
  };
}

const ThemeContext = createContext<VelmuxTheme | null>(null);

/**
 * Resolves the active palette from the persisted preference and the system
 * scheme. Dark is the default when the system reports no preference.
 */
export function ThemeProvider({children}: {children: React.ReactNode}) {
  const themeMode = useSettingsStore(state => state.themeMode);
  const systemScheme = useColorScheme();

  const theme = useMemo<VelmuxTheme>(() => {
    const resolved: ResolvedThemeMode =
      themeMode === 'system'
        ? systemScheme === 'light'
          ? 'light'
          : 'dark'
        : themeMode;
    return buildTheme(resolved);
  }, [systemScheme, themeMode]);

  return <ThemeContext.Provider value={theme}>{children}</ThemeContext.Provider>;
}

export function useTheme(): VelmuxTheme {
  const theme = useContext(ThemeContext);
  if (!theme) {
    throw new Error('useTheme must be used within ThemeProvider');
  }
  return theme;
}
