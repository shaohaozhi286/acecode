## ADDED Requirements

### Requirement: Supported Mermaid fences render as terminal diagrams
The terminal Markdown formatter SHALL render fenced blocks whose normalized language is `mermaid` as Unicode terminal diagrams when their body belongs to a supported diagram family.

#### Scenario: Flowchart renders inline
- **WHEN** a Mermaid fence contains a valid `graph` or `flowchart` diagram
- **THEN** the terminal displays its nodes, labels, connections, arrowheads, and declared direction as Unicode art

#### Scenario: State diagram renders inline
- **WHEN** a Mermaid fence contains a valid `stateDiagram` or `stateDiagram-v2` diagram
- **THEN** the terminal displays states, start/end markers, choices, descriptions, and transitions as Unicode art

#### Scenario: Class diagram renders inline
- **WHEN** a Mermaid fence contains a valid `classDiagram` diagram
- **THEN** the terminal displays class names, member compartments, relationship labels, line styles, cardinalities, and endpoints as Unicode art

#### Scenario: Entity relationship diagram renders inline
- **WHEN** a Mermaid fence contains a valid `erDiagram` diagram
- **THEN** the terminal displays entity names, attribute compartments, relationships, cardinalities, and identifying styles as Unicode art

#### Scenario: Sequence diagram renders inline
- **WHEN** a Mermaid fence contains a valid `sequenceDiagram` diagram
- **THEN** the terminal displays participants, lifelines, messages, notes, autonumbering, and supported control blocks as Unicode art

### Requirement: Mermaid output respects terminal display width
The renderer MUST measure UTF-8 labels and canvas geometry in terminal display columns and SHALL NOT return inline diagram art wider than the supplied content width.

#### Scenario: Wide-character label remains aligned
- **WHEN** a supported diagram contains Chinese or other double-width characters and fits the supplied width
- **THEN** opposite box borders and connected routes remain column-aligned

#### Scenario: Diagram exceeds available width
- **WHEN** the calculated diagram canvas is wider than the supplied content width
- **THEN** the renderer displays a width-bounded source fallback and a concise explanation that the diagram is too wide

### Requirement: Unsupported or unsafe Mermaid remains readable
The renderer SHALL preserve the original Mermaid source in a framed, width-bounded fallback whenever the diagram cannot be rendered safely and faithfully.

#### Scenario: Unsupported diagram family
- **WHEN** a Mermaid fence begins with a diagram family outside the supported five
- **THEN** the terminal displays the original source in a fallback box identifying its Mermaid family

#### Scenario: Recognized diagram has unsupported syntax
- **WHEN** a recognized diagram contains an invalid or unsupported structural statement
- **THEN** the terminal displays the complete original source rather than a misleading partial diagram

#### Scenario: Diagram exceeds complexity cap
- **WHEN** a diagram exceeds a node, edge, group, nesting, label, or canvas safety cap
- **THEN** the terminal displays the original source fallback and remains responsive

### Requirement: Existing fenced code behavior remains unchanged
The Markdown formatter MUST continue to use its existing syntax-highlighted or monochrome source presentation for non-Mermaid fences and blank Mermaid bodies.

#### Scenario: Ordinary code fence
- **WHEN** a fenced block language is not `mermaid`
- **THEN** the terminal renders it through the existing code-block path with its language label and current colors

#### Scenario: Blank Mermaid fence
- **WHEN** a Mermaid fence has an empty or whitespace-only body
- **THEN** the terminal uses the existing code-block presentation and does not produce an empty diagram canvas

### Requirement: Renderer provenance is retained
The repository MUST identify the terminal renderer as an adaptation of Grok Build's Apache-2.0 implementation and retain the applicable copyright and license notice.

#### Scenario: Source distribution includes renderer
- **WHEN** ACECode source containing the adapted renderer is distributed
- **THEN** the renderer source header and repository third-party notice identify the upstream project and Apache-2.0 license
