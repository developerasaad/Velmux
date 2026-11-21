import {createNavigationContainerRef} from '@react-navigation/native';

import type {RootStackParamList} from './types';

export const navigationRef = createNavigationContainerRef<RootStackParamList>();

/** Jumps to the Settings tab (used by the engine-unavailable banner). */
export function navigateToSettingsTab(): void {
  if (navigationRef.isReady()) {
    navigationRef.navigate('Tabs', {screen: 'Settings'});
  }
}
