---
name: plan
description: Author a short engineering plan before implementation. Captures goal, constraints, approach, and rollout for any non-trivial change.
category: engineering
platforms: [windows, macos, linux]
tags: [planning, workflow]
---

# Plan

Produce a concise, actionable engineering plan before writing code for a non-trivial change. A good plan is short (under one page), names the goal, and makes the approach legible to both the user and future you.

## When to use

- User asks for a "plan", "design", "proposal", or "strategy"
- A task spans more than ~3 files or ~2 commits
- The approach is non-obvious or has plausible alternatives worth naming

If the task is a small fix or a clear one-liner, skip the plan and implement directly.

## Output format

Write the plan as markdown into `.acecode/plans/<slug>.md`. `<slug>` is a kebab-case summary of the goal (for example `refactor-auth-middleware`). Create the `.acecode/plans/` directory if it does not exist.

The plan must contain these sections, in order:

### Goal
One or two sentences stating what "done" looks like for the user. Use concrete, testable language ("session tokens stored encrypted at rest"), not mood words ("improve auth").

### Constraints
Bullet list of hard constraints that shape the approach:
- Existing APIs that must not change
- Performance budgets
- Deadlines or release windows
- Dependencies you cannot add

### Approach
Numbered steps, in execution order. Each step is a sentence or two — not code. If a step requires a choice, name the choice and the rejected alternative in one line.

### Risks
Bullet list of things that could go wrong and the mitigation for each. Focus on risks specific to this plan, not generic software risks ("might have bugs").

### Validation
How you (or the user) will know it worked. Prefer: a test that did not exist before, a metric you can read, a manual scenario the user can click through.

## Rules

- Keep the plan under 300 words. If it wants to grow longer, the task is either too large (split it) or you are over-designing (cut).
- Do not include code samples unless a one-line snippet clarifies an API choice.
- Do not list files you will edit — that belongs in the commit, not the plan.
- After writing, print the path of the created file and a two-sentence summary back to the user. Wait for confirmation before starting implementation.

## When invoked via `/plan`

If the user typed `/plan <goal>`, treat `<goal>` as the seed for the Goal section and proceed. If `/plan` was called with no argument, ask the user what they want to plan before writing anything.
