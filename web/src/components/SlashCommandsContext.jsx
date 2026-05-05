// SlashCommandsContext:跨组件共享 GET /api/commands 的结果。
// Provider 接收 workspaceHash,workspace 切换时自动 refetch(workspace 项目里的
// .agent/skills 才能跟着切到的 workspace 一起出现)。
// SkillsPanel 启停 skill 后调 invalidate() 触发 refetch。

import { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { flattenCommands } from '../lib/slashCommands.js';

const SlashCommandsContext = createContext({
  commands: [],
  loading: false,
  error: null,
  invalidate: () => {},
});

export function SlashCommandsProvider({ workspaceHash = '', children }) {
  const [commands, setCommands] = useState([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);
  const reqIdRef = useRef(0);

  const fetchOnce = useCallback(async () => {
    const reqId = ++reqIdRef.current;
    setLoading(true);
    try {
      const payload = await api.listCommands(workspaceHash);
      if (reqId !== reqIdRef.current) return;
      setCommands(flattenCommands(payload));
      setError(null);
    } catch (e) {
      if (reqId !== reqIdRef.current) return;
      setError(e);
    } finally {
      if (reqId === reqIdRef.current) setLoading(false);
    }
  }, [workspaceHash]);

  useEffect(() => { fetchOnce(); }, [fetchOnce]);

  const invalidate = useCallback(() => { fetchOnce(); }, [fetchOnce]);

  return (
    <SlashCommandsContext.Provider value={{ commands, loading, error, invalidate }}>
      {children}
    </SlashCommandsContext.Provider>
  );
}

export function useSlashCommands() {
  return useContext(SlashCommandsContext);
}
