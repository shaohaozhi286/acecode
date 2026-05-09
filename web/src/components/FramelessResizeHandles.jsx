// 桌面壳 frameless 模式下的顶边 resize 命中条。
//
// 为啥需要:WebView2 child HWND 把整个 client area 都覆盖了,native 父窗口的
// WM_NCHITTEST 在子窗口区域永远不会被调到 — 即使 frameless_hit_test 算出
// 是 HTTOP 也送不到 Windows。同症的左/右/下边在 NCCALCSIZE 里给父窗口让出
// 了 8px NC 区,顶边不能让(让了 Windows 会强制画原生 titlebar,见
// chromium browser_desktop_window_tree_host_win.cc::GetClientAreaInsets 的
// 注释)。所以走 JS 自制 + 转给 native start_window_resize,镜像现有的
// aceDesktop_startWindowDrag 范式。
//
// strip 永远 render(只要进 frameless),最大化时 native 端会 IsZoomed 拒绝
// 调用 — cursor 还是 resize 样可能小骚扰,但 MVP 范围内可接受。

const RESIZE_STRIP_HEIGHT_PX = 6;
const RESIZE_CORNER_WIDTH_PX = 8;

function isFrameless() {
  return typeof window !== 'undefined'
    && window.__ACECODE_FRAMELESS_WINDOW__ === true
    && typeof window.aceDesktop_startWindowResize === 'function';
}

function makeStartResize(direction) {
  return (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    try {
      window.aceDesktop_startWindowResize(direction);
    } catch {
      // bridge 异常静默吞,native 自身也只是 ok:false
    }
  };
}

export function FramelessResizeHandles() {
  if (!isFrameless()) return null;
  // pointer-events: none 让中间空白区落回到下面的 TopBar(避免抢 TopBar 顶 6px
  // 拖动手势 — 顺便让 dev tools 鼠标事件穿透)。三个子元素再单独打开
  // pointer-events: auto 接 mousedown。
  return (
    <div
      className="fixed top-0 left-0 right-0 z-[1000] pointer-events-none"
      style={{ height: RESIZE_STRIP_HEIGHT_PX }}
      data-ace-no-window-drag="true"
    >
      <div
        onMouseDown={makeStartResize('top-left')}
        className="absolute left-0 top-0 pointer-events-auto cursor-[nwse-resize]"
        style={{ width: RESIZE_CORNER_WIDTH_PX, height: RESIZE_STRIP_HEIGHT_PX }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('top')}
        className="absolute top-0 pointer-events-auto cursor-[ns-resize]"
        style={{
          left: RESIZE_CORNER_WIDTH_PX,
          right: RESIZE_CORNER_WIDTH_PX,
          height: RESIZE_STRIP_HEIGHT_PX,
        }}
        aria-hidden="true"
      />
      <div
        onMouseDown={makeStartResize('top-right')}
        className="absolute right-0 top-0 pointer-events-auto cursor-[nesw-resize]"
        style={{ width: RESIZE_CORNER_WIDTH_PX, height: RESIZE_STRIP_HEIGHT_PX }}
        aria-hidden="true"
      />
    </div>
  );
}
