import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

// daemon 在 build 时把 web/dist/ 嵌入二进制(见 cmake/acecode_embed_assets.cmake)。
// 用 viteSingleFile 把 JS/CSS 全部 inline 进 index.html — 一来 daemon 只需要
// serve 单个文件,二来绕开 Crow keep-alive 在多资源连接复用时丢 Content-Type
// 的已知问题(同一 TCP 连接的第二个请求会回空 body / 空头)。
// 代价:bundle 不能拆 chunk,首屏 ~250KB(gzip 后 ~70KB),对内嵌部署完全 OK。
//
// dev 模式下跑 `pnpm dev` 起 Vite 5173,/api 与 /ws 自动代理到 127.0.0.1:28080。
export default defineConfig({
  plugins: [react(), tailwindcss(), viteSingleFile()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
    target: 'es2020',
    cssCodeSplit: false,
    assetsInlineLimit: 100 * 1024 * 1024, // 全部 inline
    rollupOptions: {
      output: {
        manualChunks: undefined,
        inlineDynamicImports: true,
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      '/api': { target: 'http://127.0.0.1:28080', changeOrigin: true },
      '/ws':  { target: 'ws://127.0.0.1:28080',   ws: true,         changeOrigin: true },
    },
  },
});
