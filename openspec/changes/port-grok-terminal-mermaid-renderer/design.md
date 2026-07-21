## Context

ACECode has two fundamentally different Markdown presentation environments. The terminal needs deterministic character-cell output, so its completed C++ renderer uses Grok Build's bounded parser, layout, and Unicode canvas. Browser WebUI and Desktop both execute the same React application in a real browser engine and should display the official Mermaid visual language rather than a vector tracing of that terminal canvas.

Mermaid source is untrusted model output. Rendering is asynchronous, Markdown can be replaced repeatedly while a response streams, and Mermaid's global configuration changes with the active theme. The Web implementation therefore needs strict input limits, no external-resource features, serialized calls into Mermaid, post-render sanitization, stale-result protection, and a readable source fallback.

## Goals / Non-Goals

**Goals:**

- Keep the native C++ terminal renderer and its five supported families unchanged.
- Render flowchart/graph, state, class, entity-relationship, and sequence fences with the pinned official Mermaid npm package in browser WebUI and Desktop.
- Use Mermaid's classic Dagre-based layout and official node/edge/typography rendering rather than the TUI cell grid.
- Preserve escaped source and source copying before, during, and after rendering.
- Reject unsafe or out-of-scope syntax before Mermaid can measure or load it.
- Sanitize returned SVG, enforce dimension/output caps, and present it as a Blob-backed image.
- Re-render settled diagrams for light/dark theme changes and ignore stale streaming results.

**Non-Goals:**

- Native C++ Mermaid-to-SVG conversion or a native Desktop SVG bridge.
- Pixel or layout consistency between terminal Unicode art and Web Mermaid diagrams.
- Mermaid-to-PNG/export UI, daemon REST rendering, or headless CLI conversion.
- Interactive Mermaid links, callbacks, images, HTML labels, custom CSS, directives, or arbitrary diagram families.
- Full compatibility with every feature accepted by Mermaid.js.

## Decisions

### Keep the terminal and Web renderers independent

The C++ terminal renderer remains the source of TUI output. WebUI and Desktop use official Mermaid.js directly. They share Markdown source and failure semantics, not parser internals, layout coordinates, fonts, or edge routes.

The abandoned shared-canvas SVG serializer and Desktop bridge are removed. A native reimplementation was rejected because matching Mermaid requires more than graph layout: each diagram family also owns shapes, labels, markers, styles, and measurement behavior.

### Use the pinned official Mermaid npm package

Add an exact `mermaid` version to the Web package and call the public asynchronous `mermaid.render(id, source)` API. The separate `@mermaid-js/tiny` artifact is intended for direct CDN use and is not a supported ESM project dependency; the full package is Mermaid's documented npm integration. Calls use `startOnLoad: false`, `securityLevel: 'strict'`, `htmlLabels: false`, the classic look, and the default Dagre layout.

Light and dark are explicit Mermaid themes. Because Mermaid configuration is process-global, rendering runs through one promise queue: initialize for a job's theme, parse/render that job, then advance to the next. A theme switch invalidates visible frames and schedules fresh jobs.

### Gate untrusted source before official parsing

Only complete, nonblank fences enter the renderer. The adapter caps source bytes and line count, recognizes only `graph`/`flowchart`, `stateDiagram`, `classDiagram`, `erDiagram`, and `sequenceDiagram`, and rejects YAML frontmatter, init directives, HTML delimiters, generalized/image-shape syntax, external URL schemes, custom CSS/style statements, clicks, callbacks, links, and actor menus.

This deliberately accepts less than Mermaid itself. The source remains visible on rejection, so safety failures never erase content or turn it into a misleading partial diagram.

### Sanitize output and render it as an image

The renderer never assigns Mermaid's SVG string to `innerHTML`. It parses the document as SVG, rejects parser errors and unexpected elements, removes active or external-resource attributes, permits only local fragment references such as marker URLs, validates the viewBox/dimensions, and enforces a serialized byte cap.

The sanitized document is serialized into an `image/svg+xml` Blob and displayed with an `img` element. Event-binding callbacks returned by Mermaid are never invoked. Object URLs are revoked after load/error or when a frame is invalidated.

### Hydrate stable Markdown frames

The synchronous Markdown renderer marks only complete, nonblank Mermaid fences. Each frame contains a hidden-capable render target plus the original escaped `code` element carrying existing copy metadata. A single observer scans inserted frames.

Each render owns a token. Results are discarded if React replaced the frame, its source changed, the theme changed, or a newer job superseded it. Success hides the source visually but leaves it in the DOM for copying. Any rejection, parse error, render error, unsafe SVG, or image-load error leaves or restores the source.

## Risks / Trade-offs

- **[Bundled Web size increases]** -> Pin the official package, rely on Vite's ESM bundling, and measure the production single-file bundle.
- **[Official Mermaid has security-sensitive syntax]** -> Gate source before parsing, use strict/no-HTML configuration, never bind interactions, sanitize output, and display only a Blob image.
- **[Unknown future external-resource syntax bypasses the gate]** -> Fail closed on generalized shapes, directives, HTML, styles, links, URLs, and unexpected SVG vocabulary; update the pinned dependency intentionally rather than floating.
- **[Global Mermaid configuration races]** -> Serialize all parse/render calls and include the requested theme in each queued job.
- **[Streaming produces wasted work]** -> Mark only closed fences, use per-frame job tokens, and discard disconnected/stale results.
- **[Web and TUI accept different edge cases]** -> Treat this as intentional surface-specific behavior; both preserve the original source on failure.
- **[Official render can still be computationally expensive]** -> Apply conservative byte/line caps before parsing and dimension/output caps after rendering.

## Migration Plan

1. Remove the uncommitted native SVG serializer, bridge, CMake wiring, and SVG tests while retaining the committed terminal renderer.
2. Add the pinned official package and a pure-gate/sanitize/render adapter.
3. Reuse the marked Markdown frames and replace Desktop-only hydration with Web-wide hydration.
4. Add focused package/gate/sanitization/theme/fallback tests and build the single-file Web bundle.
5. Reconfigure/rebuild Release targets so Desktop embeds the new Web assets, then visually verify light, dark, and failure paths.

Rollback removes the Web dependency, hydrator, and Mermaid-specific Web fence branch; stored Markdown and the terminal renderer require no migration.

## Open Questions

None. Native or headless SVG export remains separate future scope.
