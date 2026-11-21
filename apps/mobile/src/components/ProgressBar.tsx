import React, {useState} from 'react';
import {StyleSheet, View, ViewStyle} from 'react-native';
import Animated, {
  useAnimatedStyle,
  withTiming,
  Easing,
} from 'react-native-reanimated';

import {useTheme} from '../theme';
import {motion} from '../theme';

export interface ProgressBarProps {
  /**
   * Progress in 0..1, or a negative value when unknown. Unknown progress
   * renders an empty track — the UI never invents precision.
   */
  progress: number;
  height?: number;
}

export function ProgressBar({progress, height = 6}: ProgressBarProps) {
  const theme = useTheme();
  const [trackWidth, setTrackWidth] = useState(0);

  const known = Number.isFinite(progress) && progress >= 0 && progress <= 1;
  const clamped = known ? progress : 0;

  // Closure values (trackWidth, clamped) are tracked by Reanimated; the
  // animated style re-runs when either changes.
  const fillStyle = useAnimatedStyle(() => ({
    width: withTiming(trackWidth * clamped, {
      duration: motion.progress,
      easing: Easing.out(Easing.quad),
    }),
  }));

  return (
    <View
      style={[styles.track, {height, borderRadius: height / 2}]}
      onLayout={event => {
        const width = event.nativeEvent.layout.width;
        if (width > 0) {
          setTrackWidth(width);
        }
      }}>
      <Animated.View
        style={[
          styles.fill,
          {
            height,
            borderRadius: height / 2,
            backgroundColor: theme.colors.accent,
          },
          fillStyle,
        ]}
      />
    </View>
  );
}

interface ProgressBarStyles {
  track: ViewStyle;
  fill: ViewStyle;
}

const styles = StyleSheet.create<ProgressBarStyles>({
  track: {
    flex: 1,
    backgroundColor: 'rgba(128,128,128,0.22)',
    overflow: 'hidden',
  },
  fill: {
    position: 'absolute',
    left: 0,
    top: 0,
  },
});
