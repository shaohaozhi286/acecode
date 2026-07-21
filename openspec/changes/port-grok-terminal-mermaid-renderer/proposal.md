## Why

ACECode now renders supported fenced `mermaid` blocks as native Unicode diagrams in the terminal. The Web and Desktop surfaces should not inherit that cell-grid layout: users expect the polished shapes, spacing, routing, and typography produced by the official Mermaid renderer.

## What Changes

- Keep the existing native C++17 terminal renderer adapted from Grok Build for flowchart/graph, state, class, entity-relationship, and sequence diagrams.
- Mark complete, nonblank Mermaid fences in Web Markdown while preserving their escaped source and existing copy interaction.
- Add the pinned official `mermaid` npm package and use `mermaid.render()` with the classic Dagre-based Mermaid presentation on both browser WebUI and Desktop.
- Bound untrusted model output before Mermaid parses it, allow only the five intended diagram families, and reject directives, HTML, custom styling, links, images, and external-resource syntax.
- Render asynchronously with stale-result and theme-change protection, sanitize the returned SVG, and display it through a Blob-backed image rather than inserting SVG markup into the application DOM.
- Keep the original Mermaid source visible whenever a fence is incomplete, unsupported, unsafe, invalid, too large, or cannot be rendered.
- Remove the abandoned native C++ SVG serializer, Desktop bridge, CMake wiring, and their focused tests.

## Capabilities

### New Capabilities

- `terminal-mermaid-rendering`: Native, width-aware inline rendering and graceful fallback for Mermaid diagrams in the terminal Markdown surface.
- `web-mermaid-rendering`: Official Mermaid.js SVG rendering with bounded input and readable source fallback for browser WebUI and Desktop.

### Modified Capabilities

None.

## Impact

- Keeps the completed C++ TUI implementation under `src/markdown/` unchanged.
- Affects Web Markdown presentation, the Web entry point, styles, package manifest/lockfile, and focused Web tests.
- Adds one pinned bundled Web dependency; no daemon endpoint, native Desktop bridge, subprocess, network renderer, or C++ SVG API is introduced.
- Browser WebUI and Desktop intentionally share official Mermaid.js visuals, while terminal output remains optimized independently for character cells.
- Retains the Apache-2.0 attribution for the Grok Build terminal renderer.
