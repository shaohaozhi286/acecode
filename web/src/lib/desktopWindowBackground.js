// 桌面壳窗口打底色推送。
//
// 背景:WebView2 壳快速拖动窗口放大时,新暴露区域由 native 窗口的类背景刷
// 打底(web_host.cpp),颜色必须与前端 body 底色(--ace-bg)一致,否则浅色
// 主题闪黑 / 暗色主题闪白。native 启动默认浅色(#f5f5f2),这里在主题变化
// (含 mount 首次)时把实际主题色经 aceDesktop_setWindowBackgroundColor 推给
// native。浏览器直连 / webapp 兼容模式下 bridge 不存在 → no-op,与
// desktopNotify 的降级模式一致。
//
// 颜色单一事实源是 globals.css 的 --ace-bg:读 computed style 而不是在 JS
// 里硬编码主题→颜色映射,CSS 改底色时这里自动跟随。CSS custom property 的
// computed 值是声明字面量(可能带前导空白),normalize 后再传 — native 端
// 是严格哨兵(只收 #rrggbb),规范化是本文件的职责。

const THEME_BACKGROUND_VARIABLE = '--ace-bg';

// getComputedStyle 拿到的原始值 → '#rrggbb' 小写规范形态;认不出 → null。
// 接受:6 位 hex(带/不带 #)、3 位 CSS 短形(#f52 → #ff5522)。不接受
// rgb()/hsl()/关键字 — globals.css 的 --ace-bg 恒为 hex 字面量,出现其它
// 形态说明上游改了写法,宁可 no-op 也不传一个猜测值给 native。
export function normalizeThemeBackgroundColor(raw) {
  if (typeof raw !== 'string') return null;
  let text = raw.trim().toLowerCase();
  if (text.startsWith('#')) text = text.slice(1);
  if (!/^[0-9a-f]+$/.test(text)) return null;
  if (text.length === 3) {
    text = text.split('').map((c) => c + c).join('');
  }
  if (text.length !== 6) return null;
  return `#${text}`;
}

// 读当前主题 body 底色并推给 native。返回是否真正发起了推送(测试用)。
// bridge 返回 Promise,fire-and-forget:native 端非法/非 Windows 返回
// ok:false,前端不重试不上报 — 打底色是纯视觉增强,失败无感知代价。
export function pushWindowBackgroundColor(
  win = typeof window !== 'undefined' ? window : undefined,
  doc = typeof document !== 'undefined' ? document : undefined,
) {
  if (!win || !doc) return false;
  const bridge = win.aceDesktop_setWindowBackgroundColor;
  if (typeof bridge !== 'function') return false;

  let raw = '';
  try {
    raw = win
      .getComputedStyle(doc.documentElement)
      .getPropertyValue(THEME_BACKGROUND_VARIABLE) || '';
  } catch {
    return false; // 非浏览器环境 / documentElement 缺失,按无 bridge 处理
  }
  const color = normalizeThemeBackgroundColor(raw);
  if (!color) return false;

  try {
    const result = bridge(color);
    if (result && typeof result.catch === 'function') {
      result.catch(() => {});
    }
  } catch {
    return false; // bridge 同步抛(壳侧异常)也静默
  }
  return true;
}
