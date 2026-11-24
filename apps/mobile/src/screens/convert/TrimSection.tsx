import React from 'react';
import {StyleSheet, View} from 'react-native';

import {AppText} from '../../components/AppText';
import {Section} from '../../components/Section';
import {Stepper} from '../../components/Stepper';
import {ToggleRow} from '../../components/ToggleRow';
import {formatDuration} from '../../format/format';
import type {TrimSelection} from '../../state/convertStore';

export interface TrimSectionProps {
  /** Source length from inspection; null while loading, failed, or unknown. */
  durationSec: number | null;
  trim: TrimSelection | null;
  onSetTrim: (trim: TrimSelection | null) => void;
}

/** Clamps a mark to whole seconds inside [min, max] (min <= max). */
function clampSeconds(value: number, min: number, max: number): number {
  return Math.round(Math.min(Math.max(value, min), max));
}

/**
 * Timeline trimming. Off by default — the whole source is converted.
 * Enabling it keeps a window between two whole-second marks; the steppers'
 * min/max keep the window valid (start >= 0, end > start, end <= source
 * length), and the store's sanitizeTrim remains the last line of defense.
 */
export function TrimSection({durationSec, trim, onSetTrim}: TrimSectionProps) {
  // Steppers move in whole seconds, so the last reachable mark is the
  // floor of the source length. A source shorter than a second has no
  // valid window (end would have to exceed the source).
  const maxEndSec =
    durationSec !== null && Number.isFinite(durationSec)
      ? Math.floor(durationSec)
      : null;
  const canTrim = maxEndSec !== null && maxEndSec >= 1;
  const active = trim !== null && canTrim;

  // Displayed marks, clamped into the valid window first: a reused
  // request can carry marks outside the current source. Every stepper
  // interaction writes these clamped values back.
  const startSec =
    trim !== null && maxEndSec !== null
      ? clampSeconds(
          trim.startSec,
          0,
          Math.max(0, Math.min(trim.endSec ?? maxEndSec, maxEndSec) - 1),
        )
      : 0;
  const endSec =
    trim !== null && canTrim && maxEndSec !== null
      ? clampSeconds(trim.endSec ?? maxEndSec, startSec + 1, maxEndSec)
      : null;

  const handleToggle = (enabled: boolean) => {
    if (!canTrim || maxEndSec === null) {
      return;
    }
    // Enabling defaults to the whole source expressed as an explicit
    // window (the product never sends an open-ended end once the source
    // length is known); disabling drops trim so the request omits it.
    onSetTrim(enabled ? {startSec: 0, endSec: maxEndSec} : null);
  };

  const applyStart = (value: number | null) => {
    if (value === null || endSec === null) {
      return;
    }
    onSetTrim({
      startSec: clampSeconds(value, 0, endSec - 1),
      endSec,
    });
  };

  const applyEnd = (value: number | null) => {
    if (value === null || maxEndSec === null) {
      return;
    }
    onSetTrim({
      startSec,
      endSec: clampSeconds(value, startSec + 1, maxEndSec),
    });
  };

  let caption: string | null = null;
  if (!canTrim) {
    if (maxEndSec === null) {
      // A reused request can still carry a window while the source is
      // not inspected; it stays applied, it just cannot be edited here.
      caption =
        trim !== null
          ? 'Source duration unknown — the kept trim window cannot be edited.'
          : 'Source duration unknown — the whole source will be converted.';
    } else {
      caption = 'This source is shorter than a second — nothing to trim.';
    }
  }

  const lengthSec = endSec !== null ? endSec - startSec : 0;
  const lengthLabel =
    lengthSec < 60 ? `${lengthSec} s` : formatDuration(lengthSec);

  return (
    <Section
      title="Trim"
      hint="Keep only part of the source timeline.">
      <View style={styles.toggleBlock}>
        <ToggleRow
          label="Trim video"
          value={trim !== null}
          onValueChange={handleToggle}
          disabled={!canTrim}
        />
        {caption !== null && (
          <AppText variant="micro" tone="muted">
            {caption}
          </AppText>
        )}
      </View>

      {active && endSec !== null && (
        <View style={styles.controls}>
          <View style={styles.field}>
            <AppText variant="caption" tone="secondary">
              Start
            </AppText>
            <Stepper
              value={startSec}
              onChange={applyStart}
              step={1}
              min={0}
              max={endSec - 1}
              format={formatDuration}
              accessibilityLabel="trim start"
            />
          </View>

          <View style={styles.field}>
            <AppText variant="caption" tone="secondary">
              End
            </AppText>
            <Stepper
              value={endSec}
              onChange={applyEnd}
              step={1}
              min={startSec + 1}
              max={maxEndSec}
              format={formatDuration}
              accessibilityLabel="trim end"
            />
          </View>

          <View
            style={styles.summaryRow}
            accessible
            accessibilityLabel={`Keeps ${formatDuration(
              startSec,
            )} to ${formatDuration(endSec)}, ${lengthSec} seconds`}>
            <AppText variant="caption" tone="secondary">
              Keeps
            </AppText>
            <AppText variant="bodyStrong" numeric>{`${formatDuration(
              startSec,
            )} – ${formatDuration(endSec)} (${lengthLabel})`}</AppText>
          </View>
        </View>
      )}
    </Section>
  );
}

const styles = StyleSheet.create({
  toggleBlock: {
    gap: 4,
  },
  controls: {
    gap: 12,
    marginTop: 4,
  },
  field: {
    gap: 8,
  },
  summaryRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
});
