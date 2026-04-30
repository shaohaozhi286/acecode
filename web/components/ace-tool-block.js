// 工具调用块。三种状态:
//   - 进度模式: tool_start 后 → 5-line tail + +N more + 状态行
//   - summary 模式: tool_end 后 → 单行 icon · verb · object · metrics
//   - 失败折叠: success=false → summary 行下 dim 显示前 3 行 stderr
// 用户点"展开"按钮切换到完整 output 视图。
// task_complete 走特例:渲染 "Done: <summary>" 紧凑行,不可展开。

class AceToolBlock extends HTMLElement {
  constructor() {
    super();
    this.state = {
      isTaskComplete: false,
      isDone: false,
      success: null,
      title: '',
      tailLines: [],
      currentPartial: '',
      totalLines: 0,
      totalBytes: 0,
      elapsed: 0,
      summary: null,
      output: '',
      expanded: false,
    };
  }

  // 三种事件入口
  applyStart(payload) {
    this.state.isTaskComplete = !!payload.is_task_complete;
    this.state.title = payload.display_override || payload.command_preview ||
                       (payload.tool || '') + '  ' + JSON.stringify(payload.args || {});
    if (this.state.isTaskComplete) {
      const args = payload.args || {};
      this.state.summary = { object: args.summary || '完成' };
    }
    this.render();
  }
  applyUpdate(payload) {
    if (this.state.isTaskComplete) return;
    this.state.tailLines      = payload.tail_lines || [];
    this.state.currentPartial = payload.current_partial || '';
    this.state.totalLines     = payload.total_lines || 0;
    this.state.totalBytes     = payload.total_bytes || 0;
    this.state.elapsed        = payload.elapsed_seconds || 0;
    this.render();
  }
  applyEnd(payload) {
    if (this.state.isTaskComplete) return;
    this.state.isDone  = true;
    this.state.success = !!payload.success;
    this.state.summary = payload.summary || null;
    this.state.output  = payload.output || '';
    this.render();
  }

  toggleExpand() {
    this.state.expanded = !this.state.expanded;
    this.render();
  }

  render() {
    if (this.state.isTaskComplete) {
      const text = (this.state.summary && this.state.summary.object) || '完成';
      this.className = 'ace-task-complete';
      this.innerHTML = `<span>✓ Done: ${escapeHtml(text)}</span>`;
      return;
    }

    this.className = 'ace-tool-block';

    // summary 模式 (done)
    if (this.state.isDone && this.state.summary) {
      const s = this.state.summary;
      const cls = this.state.success ? 'ace-tool-summary-ok' : 'ace-tool-summary-fail';
      const metrics = (s.metrics || []).map(m => `${escapeHtml(m.label)}=${escapeHtml(m.value)}`).join(' · ');
      let html = `<div class="${cls}">${escapeHtml(s.icon || '·')} ${escapeHtml(s.verb || '')} · ${escapeHtml(s.object || '')}${metrics ? ' · ' + metrics : ''}</div>`;
      if (!this.state.success && this.state.output) {
        const lines = this.state.output.split('\n').slice(0, 3);
        html += lines.map(l => `<div class="ace-tool-stderr-line">${escapeHtml(l)}</div>`).join('');
      }
      html += `<button class="btn btn-link btn-sm p-0" data-act="expand">${this.state.expanded ? '收起' : '展开'}</button>`;
      if (this.state.expanded) {
        html += `<pre style="white-space:pre-wrap;margin:0">${escapeHtml(this.state.output)}</pre>`;
      }
      this.innerHTML = html;
      const btn = this.querySelector('[data-act="expand"]');
      if (btn) btn.onclick = () => this.toggleExpand();
      return;
    }

    // done 但无 summary → fallback 完整 output
    if (this.state.isDone) {
      const cls = this.state.success ? 'ace-tool-summary-ok' : 'ace-tool-summary-fail';
      this.innerHTML = `
        <div class="${cls}">${escapeHtml(this.state.title)}</div>
        <pre style="white-space:pre-wrap;margin:0">${escapeHtml(this.state.output)}</pre>
      `;
      return;
    }

    // 进度模式
    const tailHtml = this.state.tailLines.map(l => `<div>${escapeHtml(l)}</div>`).join('');
    const partial  = this.state.currentPartial ? `<div style="opacity:0.7">${escapeHtml(this.state.currentPartial)}</div>` : '';
    const moreHint = this.state.totalLines > this.state.tailLines.length
      ? `<div style="opacity:0.6;font-size:0.85em">+${this.state.totalLines - this.state.tailLines.length} more</div>` : '';
    this.innerHTML = `
      <div><strong>${escapeHtml(this.state.title)}</strong></div>
      <div class="text-muted small">${this.state.totalLines} lines · ${this.state.totalBytes} bytes · ${this.state.elapsed.toFixed(1)}s</div>
      ${moreHint}${tailHtml}${partial}
    `;
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

customElements.define('ace-tool-block', AceToolBlock);
