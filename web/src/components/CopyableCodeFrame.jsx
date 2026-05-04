import { clsx } from '../lib/format.js';
import { copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

export function CopyableCodeFrame({
  text = '',
  className = '',
  preClassName = '',
  children = null,
}) {
  const handleCopy = async (event) => {
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      toast({ kind: 'ok', text: '已复制代码' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  };

  return (
    <div className={clsx('ace-copyable-code', className)} data-code-copy-frame="true">
      <button
        type="button"
        className="ace-code-copy-btn"
        data-code-copy-button="true"
        title="复制代码"
        aria-label="复制代码"
        onClick={handleCopy}
      >
        <VsIcon name="copy" size={14} />
      </button>
      {children || (
        <pre className={preClassName} data-code-copy-source="true">{text}</pre>
      )}
    </div>
  );
}
