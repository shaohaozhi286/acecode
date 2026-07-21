## 1. Compact Notice Protocol

- [x] 1.1 Add stable UUIDv7 lifecycle metadata to manual and automatic compact progress/result notices
- [x] 1.2 Cover successful completion grouping and incomplete failure metadata with agent-loop tests

## 2. TUI Compact Result Folding

- [x] 2.1 Add metadata-preserving transcript callbacks and merge tagged notices in live and replayed TUI rows
- [x] 2.2 Render completed compact rows collapsed by default with Ctrl+E/Ctrl+O expansion while incomplete rows remain visible
- [x] 2.3 Add focused TUI grouping, replay, and interaction tests

## 3. TUI Compacting Animation

- [x] 3.1 Implement an elapsed-time symmetric edge-to-center compact background mask
- [x] 3.2 Integrate the exact `Compacting conversation...` row with compact lifecycle state and the existing ticker
- [x] 3.3 Add deterministic animation phase, wrap, and bounds tests

## 4. Web Compact Result Folding

- [x] 4.1 Project completed tagged compact notices into one synthetic `Context compacted` message
- [x] 4.2 Keep incomplete compact notices expanded and completed grouped details user-expandable
- [x] 4.3 Add live/history projection and component-architecture tests

## 5. Documentation and Verification

- [x] 5.1 Document compact notice metadata and TUI/Web presentation behavior
- [x] 5.2 Run strict OpenSpec validation, focused C++/Web tests, Web build, native build, and diff checks
