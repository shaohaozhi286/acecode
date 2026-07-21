## ADDED Requirements

### Requirement: Web surfaces use the official Mermaid renderer
Browser WebUI and Desktop SHALL render complete, nonblank Mermaid fences with a pinned official Mermaid.js package rather than the terminal cell-grid renderer or a native C++ SVG bridge.

#### Scenario: Supported family produces an official diagram
- **WHEN** a complete fence contains a valid flowchart/graph, state, class, entity-relationship, or sequence diagram
- **THEN** the Web surface displays SVG produced by the official Mermaid renderer with its normal shapes, routing, spacing, and typography

#### Scenario: Browser and Desktop display the same renderer output
- **WHEN** the same supported source and theme are shown in browser WebUI and Desktop
- **THEN** both surfaces use the same Web rendering adapter and official Mermaid configuration

#### Scenario: Terminal displays the same source
- **WHEN** the same Mermaid source is shown in the terminal
- **THEN** the existing native Unicode renderer remains responsible for TUI presentation and is not required to match Web layout

### Requirement: Untrusted Mermaid is gated before rendering
The Web renderer MUST enforce deterministic source limits, MUST accept only the five intended diagram families, and MUST reject syntax that can introduce directives, HTML, custom CSS, interactions, links, images, or external resources before calling Mermaid.

#### Scenario: Source contains an external-resource construct
- **WHEN** a Mermaid fence contains an image shape, URL, HTML tag, click/link/callback statement, custom style, or configuration directive
- **THEN** Mermaid is not invoked and the escaped source remains visible

#### Scenario: Source exceeds a safety cap
- **WHEN** a Mermaid fence exceeds the configured source-byte or line-count limit
- **THEN** Mermaid is not invoked and the surface remains responsive with readable source

#### Scenario: Family is unsupported
- **WHEN** a complete Mermaid fence belongs to a family outside flowchart/graph, state, class, entity-relationship, and sequence
- **THEN** the fence remains a readable source block

### Requirement: Returned SVG is inert and bounded
The Web renderer MUST sanitize Mermaid's returned SVG, MUST reject unexpected active/external-resource vocabulary, MUST enforce dimension and serialized-byte caps, and MUST display accepted output as a Blob-backed image rather than injecting the SVG string into application HTML.

#### Scenario: Mermaid output contains an unsafe element or attribute
- **WHEN** generated SVG contains scriptable markup, HTML embedding, an external reference, an event attribute, or a nonlocal resource URL
- **THEN** rendering fails closed and the original source remains visible

#### Scenario: Mermaid output is safely accepted
- **WHEN** generated SVG contains only the allowed inert Mermaid vocabulary and valid bounded dimensions
- **THEN** the sanitized document is displayed through an image object URL and Mermaid event bindings are not installed

#### Scenario: SVG exceeds an output cap
- **WHEN** generated dimensions or serialized SVG size exceed configured limits
- **THEN** no image is displayed and the original source remains visible

### Requirement: Streaming, copying, and themes remain correct
Rendered Mermaid frames SHALL retain original source copying, SHALL ignore stale asynchronous results, and SHALL regenerate for the active light or dark Mermaid theme.

#### Scenario: Fence is still streaming
- **WHEN** a Mermaid opening fence has not received a closing fence
- **THEN** it remains ordinary readable code and no Mermaid render is requested

#### Scenario: User copies a rendered fence
- **WHEN** the copy control is activated after successful rendering
- **THEN** the original Mermaid source is copied rather than SVG markup

#### Scenario: Theme changes
- **WHEN** the root application theme changes after a diagram has rendered
- **THEN** the frame discards its old image and requests official Mermaid output for the new theme

#### Scenario: Frame is replaced while rendering
- **WHEN** React replaces or updates a Mermaid frame before an older request resolves
- **THEN** the stale result cannot replace the newer frame or hide its source

#### Scenario: Official rendering fails
- **WHEN** Mermaid parsing, layout, sanitization, or image loading fails
- **THEN** the escaped original Mermaid source remains visible and copyable
