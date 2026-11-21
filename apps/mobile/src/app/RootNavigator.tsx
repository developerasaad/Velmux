import React from 'react';
import {
  DarkTheme,
  DefaultTheme,
  NavigationContainer,
  type Theme,
} from '@react-navigation/native';
import {createNativeStackNavigator} from '@react-navigation/native-stack';
import {createBottomTabNavigator} from '@react-navigation/bottom-tabs';

import {ConvertScreen} from '../screens/ConvertScreen';
import {HistoryScreen} from '../screens/HistoryScreen';
import {HomeScreen} from '../screens/HomeScreen';
import {QueueScreen} from '../screens/QueueScreen';
import {SettingsScreen} from '../screens/SettingsScreen';
import {useTheme} from '../theme';
import {navigationRef} from './navigationRef';
import type {RootStackParamList, TabParamList} from './types';

const Tab = createBottomTabNavigator<TabParamList>();
const Stack = createNativeStackNavigator<RootStackParamList>();

function Tabs() {
  const theme = useTheme();
  return (
    <Tab.Navigator
      screenOptions={{
        headerShown: false,
        tabBarActiveTintColor: theme.colors.accent,
        tabBarInactiveTintColor: theme.colors.textMuted,
        tabBarStyle: {
          backgroundColor: theme.colors.surface,
          borderTopColor: theme.colors.border,
        },
        tabBarLabelStyle: {
          fontSize: 12,
          fontWeight: '600',
        },
      }}>
      <Tab.Screen name="Home" component={HomeScreen} />
      <Tab.Screen name="Queue" component={QueueScreen} />
      <Tab.Screen name="History" component={HistoryScreen} />
      <Tab.Screen name="Settings" component={SettingsScreen} />
    </Tab.Navigator>
  );
}

function buildNavigationTheme(
  mode: 'dark' | 'light',
  colors: ReturnType<typeof useTheme>['colors'],
): Theme {
  const base = mode === 'dark' ? DarkTheme : DefaultTheme;
  return {
    ...base,
    dark: mode === 'dark',
    colors: {
      ...base.colors,
      primary: colors.accent,
      background: colors.bg,
      card: colors.surface,
      text: colors.textPrimary,
      border: colors.border,
      notification: colors.danger,
    },
  };
}

export function RootNavigator() {
  const theme = useTheme();
  const navigationTheme = buildNavigationTheme(theme.mode, theme.colors);

  return (
    <NavigationContainer ref={navigationRef} theme={navigationTheme}>
      <Stack.Navigator
        screenOptions={{
          headerShown: false,
          contentStyle: {backgroundColor: theme.colors.bg},
        }}>
        <Stack.Screen name="Tabs" component={Tabs} />
        <Stack.Screen
          name="Convert"
          component={ConvertScreen}
          options={{
            headerShown: true,
            headerBackTitle: 'Back',
            title: 'Convert',
          }}
        />
      </Stack.Navigator>
    </NavigationContainer>
  );
}
