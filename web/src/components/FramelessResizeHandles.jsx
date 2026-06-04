// 桌面壳 frameless 模式下的 resize 命中条。
//
// 为啥需要:WebView2 child HWND 把整个 client area 都覆盖了,native 父窗口的
// WM_NCHITTEST 在子窗口区域永远不会被调到 — 即使 frameless_hit_test 算出
// 是 HTTOP 也送不到 Windows。同症的左/右/下边在 NCCALCSIZE 里给父窗口让出
// 了 8px NC 区,顶边不能让(让了 Windows 会强制画原生 titlebar,见
// chromium browser_desktop_window_tree_host_win.cc::GetClientAreaInsets 的
// 注释)。所以走 JS 自制 + 转给 native start_window_resize,镜像现有的
// aceDesktop_startWindowDrag 范式。
//
// strip 永远 render(只要进 frameless),最大化时 native 端会拒绝调用。

import { nativePointerEvent } from './WindowControls.jsx';
import { FRAMELESS_RESIZE_ACTION, framelessResizeMouseDownAction } from '../lib/framelessResize.js';

const RESIZE_STRIP_HEIGHT_PX = 6;
const RESIZE_STRIP_WIDTH_PX = 6;
const RESIZE_CORNER_SIZE_PX = 10;

function isFrameless() {
  return typeof window !== 'undefined'
    && window.__ACECODE_FRAMELESS_WINDOW__ === true
    && typeof window.aceDesktop_startWindowResize === 'function';
}

function makeStartResize(direction) {
  return (event) => {
    const action = framelessResizeMouseDownAction({
      direction,
      button: event.button,
      detail: event.detail,
      canToggleMaximize: typeof window.aceDesktop_toggleMaximizeWindow === 'function',
    });
    if (action === FRAMELESS_RESIZE_ACTION.IGNORE) return;
    event.preventDefault();
    if (action === FRAMELESS_RESIZE_ACTION.TOGGLE_MAXIMIZE) {
      window.aceDesktop_toggleMaximizeWindow();
      return;
    }
    try {
      window.aceDesktop_startWindowResize(direction, nativePointerEvent(event));
    } catch {
      // bridge 异常静默吞,native 自身也只是 ok:false
    }
  };
}

export function FramelessResizeHandles() {
  if (!isFrameless()) return null;
  // pointer-events: none 让中间空白区落回到底层内容。各边/角单独打开
  // pointer-events: auto 接 mousedown。
  return (
    <div
      className="fixed inset-0 z-[1000] pointer-events-none"
      data-ace-no-window-drag="true"
    >
      {/* Edges */}
      <div
        onMouseDown={makeStartResize('top')}
        className="absolute top-0 pointer-events-auto cursor-[ns-resize]"
        style={{
          left: RESIZE_CORNER_SIZE_PX,
          right: RESIZE_CORNER_SIZE_PX,
          height: RESIZE_STRIP_HEIGHT_PX,
        }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('bottom')}
        className="absolute bottom-0 pointer-events-auto cursor-[ns-resize]"
        style={{
          left: RESIZE_CORNER_SIZE_PX,
          right: RESIZE_CORNER_SIZE_PX,
          height: RESIZE_STRIP_HEIGHT_PX,
        }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('left')}
        className="absolute left-0 pointer-events-auto cursor-[ew-resize]"
        style={{
          top: RESIZE_CORNER_SIZE_PX,
          bottom: RESIZE_CORNER_SIZE_PX,
          width: RESIZE_STRIP_WIDTH_PX,
        }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('right')}
        className="absolute right-0 pointer-events-auto cursor-[ew-resize]"
        style={{
          top: RESIZE_CORNER_SIZE_PX,
          bottom: RESIZE_CORNER_SIZE_PX,
          width: RESIZE_STRIP_WIDTH_PX,
        }}
        aria-hidden="true"
      />

      {/* Corners */}
      <div
        onMouseDown={makeStartResize('top-left')}
        className="absolute left-0 top-0 pointer-events-auto cursor-[nwse-resize]"
        style={{ width: RESIZE_CORNER_SIZE_PX, height: RESIZE_CORNER_SIZE_PX }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('top-right')}
        className="absolute right-0 top-0 pointer-events-auto cursor-[nesw-resize]"
        style={{ width: RESIZE_CORNER_SIZE_PX, height: RESIZE_CORNER_SIZE_PX }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('bottom-left')}
        className="absolute left-0 bottom-0 pointer-events-auto cursor-[nesw-resize]"
        style={{ width: RESIZE_CORNER_SIZE_PX, height: RESIZE_CORNER_SIZE_PX }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('bottom-right')}
        className="absolute right-0 bottom-0 pointer-events-auto cursor-[nwse-resize]"
        style={{ width: RESIZE_CORNER_SIZE_PX, height: RESIZE_CORNER_SIZE_PX }}
        aria-hidden="true"
      />
    </div>
  );
}
