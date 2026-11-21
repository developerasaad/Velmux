import React, {useEffect} from 'react';
import {StatusBar, StyleSheet, View} from 'react-native';
import {GestureHandlerRootView} from 'react-native-gesture-handler';
import {SafeAreaProvider} from 'react-native-safe-area-context';

import {navigateToSettingsTab} from './src/app/navigationRef';
import {RootNavigator} from './src/app/RootNavigator';
import {Banner} from './src/components/Banner';
import {BRIDGE_UNAVAILABLE_MESSAGE, useJobsStore} from './src/state/jobsStore';
import {ThemeProvider, useTheme} from './src/theme';

function AppContent() {
  const theme = useTheme();
  const bridgeAvailable = useJobsStore(state => state.bridgeAvailable);
  const initialized = useJobsStore(state => state.initialized);
  const init = useJobsStore(state => state.init);

  useEffect(() => {
    void init();
  }, [init]);

  return (
    <View style={styles.container}>
      <StatusBar
        barStyle={theme.mode === 'dark' ? 'light-content' : 'dark-content'}
      />
      {initialized && !bridgeAvailable && (
        <Banner
          message={`${BRIDGE_UNAVAILABLE_MESSAGE} — conversion is unavailable until the engine is integrated. Tap for diagnostics.`}
          onPress={navigateToSettingsTab}
        />
      )}
      <View style={styles.navigator}>
        <RootNavigator />
      </View>
    </View>
  );
}

function App() {
  return (
    <GestureHandlerRootView style={styles.container}>
      <SafeAreaProvider>
        <ThemeProvider>
          <AppContent />
        </ThemeProvider>
      </SafeAreaProvider>
    </GestureHandlerRootView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
  },
  navigator: {
    flex: 1,
  },
});

export default App;
