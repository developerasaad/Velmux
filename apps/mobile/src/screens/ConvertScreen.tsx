import React, {useCallback, useEffect, useRef, useState} from 'react';
import {Alert, StyleSheet, View} from 'react-native';
import {
  useNavigation,
} from '@react-navigation/native';
import {
  type NativeStackScreenProps,
} from '@react-navigation/native-stack';

import {AppText} from '../components/AppText';
import {Button} from '../components/Button';
import {Screen} from '../components/Screen';
import * as engineClient from '../native/engineClient';
import {EngineBridgeUnavailable} from '../native/engineClient';
import {BRIDGE_UNAVAILABLE_MESSAGE, useJobsStore} from '../state/jobsStore';
import {
  computeRequest,
  useConvertStore,
} from '../state/convertStore';
import type {RootStackParamList} from '../app/types';
import {AdvancedSection} from './convert/AdvancedSection';
import {AudioSection} from './convert/AudioSection';
import {EstimateCard} from './convert/EstimateCard';
import type {PlanState} from './convert/EstimateCard';
import {GoalSection} from './convert/GoalSection';
import {OutputSection} from './convert/OutputSection';
import {
  SAMPLE_PREVIEW_SECONDS,
  SamplePreviewCard,
} from './convert/SamplePreviewCard';
import type {SampleState} from './convert/SamplePreviewCard';
import {SourceInfoCard} from './convert/SourceInfoCard';
import type {InspectState} from './convert/SourceInfoCard';
import {SubtitlesSection} from './convert/SubtitlesSection';

type ConvertScreenProps = NativeStackScreenProps<RootStackParamList, 'Convert'>;

const PLAN_DEBOUNCE_MS = 500;

export function ConvertScreen({route}: ConvertScreenProps) {
  const navigation = useNavigation<ConvertScreenProps['navigation']>();
  const {source, intent, request} = route.params;

  const beginDraft = useConvertStore(state => state.beginDraft);
  const draft = useConvertStore(state => state.draft);
  const bridgeAvailable = useJobsStore(state => state.bridgeAvailable);
  const startJob = useJobsStore(state => state.startJob);

  const [inspectState, setInspectState] = useState<InspectState>({
    status: 'idle',
  });
  const [planState, setPlanState] = useState<PlanState>({status: 'idle'});
  const [sampleState, setSampleState] = useState<SampleState>({
    status: 'idle',
  });
  const [starting, setStarting] = useState(false);

  const inspectRun = useRef(0);
  const planRun = useRef(0);
  const sampleRun = useRef(0);

  // Initialize the draft when entering the workspace with a new source.
  useEffect(() => {
    if (intent === 'reuse' && request) {
      beginDraft(source, {intent: 'reuse', request});
    } else {
      beginDraft(source, {intent});
    }
  }, [beginDraft, intent, request, source]);

  const runInspect = useCallback(async () => {
    const run = inspectRun.current + 1;
    inspectRun.current = run;
    setInspectState({status: 'loading'});
    if (!engineClient.isEngineAvailable()) {
      setInspectState({
        status: 'unavailable',
        message: BRIDGE_UNAVAILABLE_MESSAGE,
      });
      return;
    }
    try {
      const info = await engineClient.inspect(source);
      if (inspectRun.current === run) {
        setInspectState({status: 'ready', info});
      }
    } catch (error) {
      if (inspectRun.current !== run) {
        return;
      }
      if (error instanceof EngineBridgeUnavailable) {
        setInspectState({status: 'unavailable', message: error.message});
        return;
      }
      setInspectState({
        status: 'error',
        message: engineClient.describeEngineError(error),
      });
    }
  }, [source]);

  useEffect(() => {
    runInspect();
  }, [runInspect]);

  // Immediate re-plan, used by the estimate retry control.
  const runPlan = useCallback(async () => {
    const computed = computeRequest(useConvertStore.getState().draft);
    if (computed === null) {
      setPlanState({status: 'idle'});
      return;
    }
    if (!engineClient.isEngineAvailable()) {
      setPlanState({
        status: 'unavailable',
        message: BRIDGE_UNAVAILABLE_MESSAGE,
      });
      return;
    }
    const run = planRun.current + 1;
    planRun.current = run;
    setPlanState({status: 'estimating'});
    try {
      const plan = await engineClient.plan(computed);
      if (planRun.current === run) {
        setPlanState({status: 'ready', ...plan});
      }
    } catch (error) {
      if (planRun.current !== run) {
        return;
      }
      if (error instanceof EngineBridgeUnavailable) {
        setPlanState({status: 'unavailable', message: error.message});
        return;
      }
      setPlanState({
        status: 'error',
        message: engineClient.describeEngineError(error),
      });
    }
  }, []);

  // Debounced re-plan whenever the request changes.
  useEffect(() => {
    const computed = computeRequest(draft);
    if (computed === null) {
      setPlanState({status: 'idle'});
      return;
    }
    if (!engineClient.isEngineAvailable()) {
      setPlanState({
        status: 'unavailable',
        message: BRIDGE_UNAVAILABLE_MESSAGE,
      });
      return;
    }

    const run = planRun.current + 1;
    planRun.current = run;
    setPlanState({status: 'estimating'});

    const timer = setTimeout(() => {
      (async () => {
        try {
          const plan = await engineClient.plan(computed);
          if (planRun.current === run) {
            setPlanState({status: 'ready', ...plan});
          }
        } catch (error) {
          if (planRun.current !== run) {
            return;
          }
          if (error instanceof EngineBridgeUnavailable) {
            setPlanState({status: 'unavailable', message: error.message});
            return;
          }
          setPlanState({
            status: 'error',
            message: engineClient.describeEngineError(error),
          });
        }
      })();
    }, PLAN_DEBOUNCE_MS);

    return () => {
      clearTimeout(timer);
    };
  }, [draft]);

  // Sample preview (spec §20): generates a short sample over the densest
  // section of the source with the exact request the Convert action sends.
  const runSample = useCallback(async () => {
    const computed = computeRequest(useConvertStore.getState().draft);
    if (computed === null) {
      return;
    }
    if (!engineClient.isEngineAvailable()) {
      setSampleState({
        status: 'unavailable',
        message: BRIDGE_UNAVAILABLE_MESSAGE,
      });
      return;
    }
    const run = sampleRun.current + 1;
    sampleRun.current = run;
    setSampleState({status: 'generating'});
    try {
      const sample = await engineClient.generateSample(
        computed,
        SAMPLE_PREVIEW_SECONDS,
      );
      if (sampleRun.current === run) {
        setSampleState({status: 'ready', sample});
      }
    } catch (error) {
      if (sampleRun.current !== run) {
        return;
      }
      if (error instanceof EngineBridgeUnavailable) {
        setSampleState({status: 'unavailable', message: error.message});
        return;
      }
      setSampleState({
        status: 'error',
        message: engineClient.describeEngineError(error),
      });
    }
  }, []);

  // A sample reflects the current settings: any draft change invalidates
  // it (the same rule the estimate follows) and discards an in-flight
  // generation, so a preview is never shown next to changed settings.
  useEffect(() => {
    sampleRun.current += 1;
    setSampleState(previous =>
      previous.status === 'idle' ? previous : {status: 'idle'},
    );
  }, [draft]);

  const planInfeasible =
    planState.status === 'ready' && planState.feasible === false;
  const canStart = bridgeAvailable && draft.source !== null && !planInfeasible;

  const handleStart = useCallback(async () => {
    const computed = computeRequest(useConvertStore.getState().draft);
    if (computed === null) {
      return;
    }
    setStarting(true);
    try {
      await startJob(computed);
      navigation.goBack();
    } catch (error) {
      setStarting(false);
      Alert.alert(
        'Could not start the job',
        engineClient.describeEngineError(error),
      );
    }
  }, [navigation, startJob]);

  let startFootnote: string | undefined;
  if (planInfeasible) {
    startFootnote =
      'The engine considers the current settings infeasible — review the estimate above.';
  } else if (!bridgeAvailable) {
    startFootnote =
      'Starting jobs requires the native engine, which is not linked in this build.';
  }

  return (
    <Screen
      scroll
      footer={
        <View style={styles.footer}>
          <Button
            label="Start conversion"
            variant="primary"
            size="large"
            loading={starting}
            disabled={!canStart}
            onPress={() => {
              handleStart();
            }}
          />
          {startFootnote !== undefined && (
            <AppText variant="micro" tone="muted" style={styles.footnote}>
              {startFootnote}
            </AppText>
          )}
        </View>
      }>
      <SourceInfoCard
        sourceName={source.displayName}
        inspectState={inspectState}
        onRetry={() => {
          runInspect();
        }}
      />
      <GoalSection />
      <OutputSection />
      <AudioSection />
      <SubtitlesSection />
      <EstimateCard planState={planState} onRetry={() => {
        runPlan();
      }} />
      {planState.status === 'ready' && (
        <SamplePreviewCard
          sourceName={source.displayName}
          sampleState={sampleState}
          onGenerate={runSample}
        />
      )}
      <AdvancedSection />
    </Screen>
  );
}

const styles = StyleSheet.create({
  footer: {
    gap: 8,
  },
  footnote: {
    textAlign: 'center',
  },
});
