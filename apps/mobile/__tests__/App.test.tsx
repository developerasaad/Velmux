import {
  formatBytes,
  formatDuration,
  formatPercent,
  formatSavedPercent,
  parseTargetSizeBytes,
} from '../src/format/format';

describe('format helpers', () => {
  it('formats byte counts with units', () => {
    expect(formatBytes(null)).toBe('—');
    expect(formatBytes(512)).toBe('512 B');
    expect(formatBytes(2048)).toBe('2 KB');
    expect(formatBytes(38_100_000)).toBe('38.1 MB');
    expect(formatBytes(1_240_000_000)).toBe('1.2 GB');
  });

  it('formats durations', () => {
    expect(formatDuration(null)).toBe('—');
    expect(formatDuration(59)).toBe('0:59');
    expect(formatDuration(754)).toBe('12:34');
    expect(formatDuration(5025)).toBe('1:23:45');
  });

  it('formats progress percentages without inventing precision', () => {
    expect(formatPercent(-1)).toBe('—');
    expect(formatPercent(0.4249)).toBe('42%');
    expect(formatPercent(1)).toBe('100%');
  });

  it('formats space saved between input and output', () => {
    expect(formatSavedPercent(100, 30)).toBe('70%');
    expect(formatSavedPercent(null, 30)).toBe('—');
    expect(formatSavedPercent(30, 100)).toBe('—');
  });

  it('parses target sizes in decimal units', () => {
    expect(parseTargetSizeBytes('', 'MB')).toBeNull();
    expect(parseTargetSizeBytes('25', 'MB')).toBe(25_000_000);
    expect(parseTargetSizeBytes('1.5', 'GB')).toBe(1_500_000_000);
    expect(parseTargetSizeBytes('abc', 'MB')).toBeNull();
  });
});
