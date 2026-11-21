/**
 * Velmux design tokens.
 *
 * Dark-first premium neutrals with a warm copper accent. No indigo/blue.
 * The token set is intentionally small and stable: screens compose tokens,
 * they never invent ad-hoc values.
 */

export interface Palette {
  bg: string;
  surface: string;
  elevated: string;
  border: string;
  textPrimary: string;
  textSecondary: string;
  textMuted: string;
  accent: string;
  accentPressed: string;
  onAccent: string;
  success: string;
  warning: string;
  danger: string;
  info: string;
}

export const darkPalette: Palette = {
  bg: '#0C0D10',
  surface: '#15171C',
  elevated: '#1B1E24',
  border: '#26292F',
  textPrimary: '#ECEDEE',
  textSecondary: '#9BA0A8',
  textMuted: '#6A6F77',
  accent: '#E8833A',
  accentPressed: '#D2722F',
  onAccent: '#14100C',
  success: '#4CAF7D',
  warning: '#D9A441',
  danger: '#E25B5B',
  info: '#6FA8B8',
};

export const lightPalette: Palette = {
  bg: '#F7F7F8',
  surface: '#FFFFFF',
  elevated: '#FFFFFF',
  border: '#E3E4E6',
  textPrimary: '#17181C',
  textSecondary: '#55585E',
  textMuted: '#8B8E94',
  accent: '#C96A28',
  accentPressed: '#B25A1F',
  onAccent: '#FFFFFF',
  success: '#2E8B5F',
  warning: '#A87718',
  danger: '#C24141',
  info: '#3E7484',
};

/** Spacing scale: 4 / 8 / 12 / 16 / 20 / 24 / 32. */
export const spacing = {
  4: 4,
  8: 8,
  12: 12,
  16: 16,
  20: 20,
  24: 24,
  32: 32,
} as const;

export type SpacingStep = keyof typeof spacing;

/** Corner radii. */
export const radii = {
  sm: 8,
  md: 12,
  lg: 16,
  pill: 999,
} as const;

export type RadiusStep = keyof typeof radii;

export type TypeVariant = keyof typeof type;

/** Compact type scale. System font stack only; no custom font files. */
export const type = {
  titleLarge: {fontSize: 22, lineHeight: 28, fontWeight: '600'},
  title: {fontSize: 17, lineHeight: 22, fontWeight: '600'},
  body: {fontSize: 15, lineHeight: 20, fontWeight: '400'},
  bodyStrong: {fontSize: 15, lineHeight: 20, fontWeight: '600'},
  caption: {fontSize: 13, lineHeight: 17, fontWeight: '400'},
  captionStrong: {fontSize: 13, lineHeight: 17, fontWeight: '600'},
  micro: {fontSize: 12, lineHeight: 15, fontWeight: '500'},
} as const;

/** Numeric text (sizes, rates, percentages) uses tabular figures. */
export const numericFontVariant = ['tabular-nums'] as const;

/** Motion: short, restrained, purposeful. */
export const motion = {
  press: 110,
  progress: 240,
} as const;

/** Minimum touch target for every interactive element. */
export const minTouchTarget = 44;
