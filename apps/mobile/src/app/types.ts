import type {NavigatorScreenParams} from '@react-navigation/native';

import type {ConversionRequest, MediaSource} from '../native/NativeVelmux';
import type {BeginDraftOptions} from '../state/convertStore';

export type TabParamList = {
  Home: undefined;
  Queue: undefined;
  History: undefined;
  Settings: undefined;
};

export type ConvertIntent = NonNullable<BeginDraftOptions['intent']>;

export type RootStackParamList = {
  Tabs: NavigatorScreenParams<TabParamList> | undefined;
  Convert: {
    source: MediaSource;
    intent: ConvertIntent;
    /** Present when reusing a previous request from History. */
    request?: ConversionRequest;
  };
};
