/**
 * Formatting helpers. All numeric output is rendered with tabular figures
 * by the UI (see AppText `numeric`), these functions only produce strings.
 */

const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB'] as const;

/** Formats a byte count for display; null/unknown renders as an em dash. */
export function formatBytes(bytes: number | null | undefined): string {
  if (bytes === null || bytes === undefined || !Number.isFinite(bytes)) {
    return '—';
  }
  if (bytes < 1024) {
    return `${Math.round(bytes)} B`;
  }
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < BYTE_UNITS.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  const decimals = value >= 100 ? 0 : 1;
  return `${value.toFixed(decimals)} ${BYTE_UNITS[unitIndex]}`;
}

/** Formats a duration in seconds as h:mm:ss / m:ss. */
export function formatDuration(seconds: number | null | undefined): string {
  if (
    seconds === null ||
    seconds === undefined ||
    !Number.isFinite(seconds) ||
    seconds < 0
  ) {
    return '—';
  }
  const total = Math.round(seconds);
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const secs = total % 60;
  const mm = hours > 0 ? String(minutes).padStart(2, '0') : String(minutes);
  const ss = String(secs).padStart(2, '0');
  return hours > 0 ? `${hours}:${mm}:${ss}` : `${mm}:${ss}`;
}

/**
 * Formats a 0..1 progress fraction as an integer percentage.
 * Values outside the known range render as an em dash (never invented).
 */
export function formatPercent(progress: number | null | undefined): string {
  if (
    progress === null ||
    progress === undefined ||
    !Number.isFinite(progress) ||
    progress < 0 ||
    progress > 1
  ) {
    return '—';
  }
  return `${Math.round(progress * 100)}%`;
}

/** Formats a bit rate in bits per second. */
export function formatBitRate(bitsPerSecond: number | null | undefined): string {
  if (
    bitsPerSecond === null ||
    bitsPerSecond === undefined ||
    !Number.isFinite(bitsPerSecond) ||
    bitsPerSecond <= 0
  ) {
    return '—';
  }
  if (bitsPerSecond >= 1_000_000) {
    return `${(bitsPerSecond / 1_000_000).toFixed(1)} Mb/s`;
  }
  return `${Math.round(bitsPerSecond / 1000)} kb/s`;
}

/** Formats a sample rate in Hz. */
export function formatSampleRate(hz: number | null | undefined): string {
  if (hz === null || hz === undefined || !Number.isFinite(hz) || hz <= 0) {
    return '—';
  }
  const khz = hz / 1000;
  const label = Number.isInteger(khz) ? String(khz) : khz.toFixed(1);
  return `${label} kHz`;
}

/** Space saved between input and output, as a display percentage. */
export function formatSavedPercent(
  inputBytes: number | null | undefined,
  outputBytes: number | null | undefined,
): string {
  if (
    inputBytes === null ||
    inputBytes === undefined ||
    outputBytes === null ||
    outputBytes === undefined ||
    inputBytes <= 0 ||
    outputBytes < 0 ||
    outputBytes > inputBytes
  ) {
    return '—';
  }
  return `${Math.round((1 - outputBytes / inputBytes) * 100)}%`;
}

/**
 * Parses a user-entered target size. Uses decimal units (1 MB = 10^6 bytes),
 * matching how upload limits are expressed.
 */
export function parseTargetSizeBytes(
  text: string,
  unit: 'MB' | 'GB',
): number | null {
  const trimmed = text.trim();
  if (trimmed.length === 0) {
    return null;
  }
  const value = Number(trimmed.replace(',', '.'));
  if (!Number.isFinite(value) || value <= 0) {
    return null;
  }
  const multiplier = unit === 'GB' ? 1_000_000_000 : 1_000_000;
  return Math.round(value * multiplier);
}

/** Parses a positive integer from user input; null when absent or invalid. */
export function parsePositiveInt(text: string): number | null {
  const trimmed = text.trim();
  if (trimmed.length === 0) {
    return null;
  }
  if (!/^\d+$/.test(trimmed)) {
    return null;
  }
  const value = Number(trimmed);
  return Number.isSafeInteger(value) && value > 0 ? value : null;
}
