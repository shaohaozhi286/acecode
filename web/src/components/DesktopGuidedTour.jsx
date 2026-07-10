import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Joyride } from 'react-joyride';
import {
  buildDesktopGuidedTourSteps,
  desktopGuidedTourLostRequiredTarget,
  desktopGuidedTourTerminalAction,
} from '../lib/desktopGuidedTour.js';

function useReducedMotion() {
  const [reduced, setReduced] = useState(false);

  useEffect(() => {
    if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return undefined;
    const query = window.matchMedia('(prefers-reduced-motion: reduce)');
    const update = () => setReduced(query.matches);
    update();
    query.addEventListener?.('change', update);
    return () => query.removeEventListener?.('change', update);
  }, []);

  return reduced;
}

export function DesktopGuidedTour({ run, hasModel, onDismiss, onAbort }) {
  const reducedMotion = useReducedMotion();
  const terminalHandledRef = useRef(false);
  const steps = useMemo(() => buildDesktopGuidedTourSteps({ hasModel }), [hasModel]);

  useEffect(() => {
    if (run) terminalHandledRef.current = false;
  }, [run]);

  useEffect(() => {
    if (!run) return undefined;
    const dismissOnEscape = (event) => {
      if (event.key !== 'Escape' || terminalHandledRef.current) return;
      terminalHandledRef.current = true;
      event.preventDefault();
      event.stopPropagation();
      event.stopImmediatePropagation?.();
      onDismiss?.({ openModels: false });
    };
    const blockContextMenu = (event) => {
      event.preventDefault();
      event.stopPropagation();
    };
    document.addEventListener('keydown', dismissOnEscape, true);
    document.addEventListener('contextmenu', blockContextMenu, true);
    return () => {
      document.removeEventListener('keydown', dismissOnEscape, true);
      document.removeEventListener('contextmenu', blockContextMenu, true);
    };
  }, [onDismiss, run]);

  const onEvent = useCallback((event) => {
    if (terminalHandledRef.current) return;
    if (desktopGuidedTourLostRequiredTarget(event)) {
      terminalHandledRef.current = true;
      onAbort?.();
      return;
    }
    const action = desktopGuidedTourTerminalAction(event, { hasModel });
    if (!action) return;
    terminalHandledRef.current = true;
    onDismiss?.({ openModels: action.openModels });
  }, [hasModel, onAbort, onDismiss]);

  const options = useMemo(() => ({
    arrowColor: 'var(--ace-surface)',
    backgroundColor: 'var(--ace-surface)',
    blockTargetInteraction: true,
    buttons: ['back', 'close', 'primary', 'skip'],
    closeButtonAction: 'skip',
    disableFocusTrap: false,
    dismissKeyAction: false,
    overlayClickAction: false,
    overlayColor: 'rgba(0, 0, 0, 0.58)',
    primaryColor: '#2563eb',
    scrollDuration: reducedMotion ? 0 : 260,
    scrollOffset: 12,
    showProgress: true,
    skipBeacon: true,
    skipScroll: reducedMotion,
    spotlightPadding: 6,
    spotlightRadius: 8,
    targetWaitTimeout: 1200,
    textColor: 'var(--ace-fg)',
    width: 'min(360px, calc(100vw - 32px))',
    zIndex: 11000,
  }), [reducedMotion]);

  const styles = useMemo(() => ({
    buttonBack: {
      borderRadius: 6,
      color: 'var(--ace-fg-mute)',
      fontSize: 12,
      padding: '7px 10px',
    },
    buttonClose: {
      color: 'currentColor',
      height: 14,
      padding: 6,
      right: 10,
      top: 10,
      width: 14,
    },
    buttonPrimary: {
      backgroundColor: 'var(--ace-accent)',
      borderRadius: 6,
      color: '#fff',
      fontSize: 12,
      fontWeight: 600,
      padding: '8px 13px',
    },
    buttonSkip: {
      color: 'var(--ace-fg-mute)',
      fontSize: 12,
      padding: '7px 8px',
    },
    spotlight: {
      transition: reducedMotion ? 'none' : undefined,
    },
    tooltip: {
      border: '1px solid var(--ace-border)',
      borderRadius: 10,
      boxShadow: 'var(--ace-shadow-lg)',
      fontFamily: 'inherit',
      padding: 0,
    },
    tooltipContainer: {
      textAlign: 'left',
    },
    tooltipContent: {
      color: 'var(--ace-fg-2)',
      fontSize: 13,
      lineHeight: 1.6,
      padding: '8px 18px 14px',
    },
    tooltipFooter: {
      borderTop: '1px solid var(--ace-border)',
      gap: 4,
      marginTop: 0,
      padding: '10px 12px',
    },
    tooltipTitle: {
      color: 'var(--ace-fg)',
      fontSize: 15,
      fontWeight: 650,
      padding: '16px 44px 2px 18px',
    },
  }), [reducedMotion]);

  return (
    <Joyride
      continuous
      initialStepIndex={0}
      locale={{
        back: '上一步',
        close: '关闭指引',
        last: hasModel ? '完成' : '去配置模型',
        next: '下一步',
        nextWithProgress: '下一步（{current}/{total}）',
        open: '打开新手指引',
        skip: '跳过指引',
      }}
      onEvent={onEvent}
      options={options}
      run={run}
      scrollToFirstStep={!reducedMotion}
      steps={steps}
      styles={styles}
    />
  );
}
