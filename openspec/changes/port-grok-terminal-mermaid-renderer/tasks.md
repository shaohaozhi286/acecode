## 1. Renderer foundation

- [x] 1.1 Add the pure C++ Mermaid renderer API, semantic output roles, Unicode display-width helpers, and deterministic resource limits
- [x] 1.2 Port the bounded Unicode canvas, line-style composition, label cleanup/wrapping, and raw-source fallback
- [x] 1.3 Add the renderer source to ACECode's focused testable CMake boundary

## 2. Graph-family parsing and layout

- [x] 2.1 Port flowchart/graph parsing for nodes, shapes, chains, labels, link styles, directions, endpoint markers, and subgraphs
- [x] 2.2 Port rank ordering, TD/LR placement, edge routing, reverse-direction transforms, and grouped-frame rendering
- [x] 2.3 Port state diagram parsing onto the graph layout, including markers, choices, aliases, descriptions, composites, and transitions

## 3. Structured diagram families

- [x] 3.1 Port class diagram parsing and compartment rendering, including members, annotations, generics, relationships, cardinalities, and line styles
- [x] 3.2 Port ER diagram parsing and compartment rendering, including entity aliases, attributes, relationship labels, cardinalities, and identifying styles
- [x] 3.3 Port sequence parsing and layout for participants, messages, self-messages, notes, autonumbering, and supported control blocks

## 4. Markdown integration and provenance

- [x] 4.1 Integrate nonblank `mermaid` fences into the terminal Markdown formatter with theme-aware semantic styling and ordinary-code fallback
- [x] 4.2 Add Grok Build Apache-2.0 attribution in the renderer source and repository third-party notice

## 5. Terminal verification

- [x] 5.1 Add focused unit tests for all five families, Unicode alignment, narrow-width fallback, invalid/unsupported input, safety caps, and Markdown integration
- [x] 5.2 Run focused tests, the full unit suite, code-quality checks, and a Release build
- [x] 5.3 Render representative wide and narrow diagrams through FTXUI and inspect captured terminal output

## 6. Official Web renderer

- [x] 6.1 Remove the abandoned native C++ SVG API, Desktop bridge, CMake wiring, and SVG-specific C++ tests without changing terminal output
- [x] 6.2 Pin the official Mermaid npm package and add a serialized render adapter with five-family input gates, fixed themes, sanitized SVG, dimension/output caps, and Blob-image output
- [x] 6.3 Hydrate complete nonblank Mermaid fences in browser WebUI and Desktop with source copying, readable fallback, stale-request protection, and theme re-rendering

## 7. Web verification

- [x] 7.1 Add focused tests for fence completion, source gates, official package support for all five families, SVG sanitization, dimensions, theme selection, and fallback behavior
- [x] 7.2 Run focused and full Web tests, production Web build, strict OpenSpec validation, `git diff --check`, and the C++ regression suite/Release builds (the full Web run retains one unrelated Sidebar architecture assertion failure)
- [x] 7.3 Launch the Desktop build and visually inspect official Mermaid output in light/dark themes plus invalid-source fallback
