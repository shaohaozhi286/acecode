# Skills

Skills are user-authored instruction documents that extend acecode with domain workflows (planning, debugging, deploying, etc.). Each skill is a Markdown file with YAML frontmatter that acecode discovers at startup and exposes to the LLM via **progressive disclosure** and to you via a dedicated slash command.

## Directory layout

Skills live under `~/.acecode/skills/`. The recommended layout is:

```
~/.acecode/skills/
  <category>/
    <skill-name>/
      SKILL.md            # required — frontmatter + body
      references/         # optional supporting markdown
      templates/          # optional templates
      scripts/            # optional executable helpers
      assets/             # optional binary assets
```

`<category>` is a free-form folder name (for example `engineering`, `writing`, `ops`). Skills at the top level (without a category folder) get category `"default"`. Extra root directories can be added via `config.skills.external_dirs`.

## SKILL.md format

```markdown
---
name: plan
description: Author a short engineering plan before implementation.
category: engineering           # optional, inferred from folder when omitted
platforms: [windows, macos, linux]  # optional, defaults to all
tags: [planning, workflow]      # optional
---

# Plan

... body instructions the LLM will read when the skill is activated ...
```

**Required**: `name`, `description`. Everything else is optional. `name` is normalized to kebab-case for the slash command key (so `name: My Plan` → `/my-plan`).

## How skills activate

Two activation paths, both routed through the agent loop:

1. **User invocation** — Type `/<skill-name> [optional argument]`. acecode loads the full body, appends any supporting file listing and the optional argument, and submits the result as a user message. The LLM then follows the skill's instructions.
2. **LLM discovery** — The LLM sees a `# Skills` hint in the system prompt whenever at least one skill is installed, plus two tools:
   - `skills_list` — returns `[{name, description, category}, …]`. Tier-1 metadata.
   - `skill_view` — returns the full SKILL.md body (tier-2) or a supporting file (tier-3). `{name, file_path?}`.

The LLM is expected to call `skills_list` when a task looks skill-shaped, then `skill_view` to load the chosen skill before acting.

## Configuration

`~/.acecode/config.json`:

```json
{
  "skills": {
    "disabled": ["plan"],
    "external_dirs": ["~/work/team-skills"]
  }
}
```

- `disabled` — skill names to hide even if the files are on disk.
- `external_dirs` — extra scan roots. `~` and `${ENV}` are expanded.

Both fields default to empty and the whole block may be omitted.

## Runtime commands

- `/skills` or `/skills list` — show installed skills grouped by category.
- `/skills reload` — rescan disk and re-register slash commands. Use after editing a SKILL.md.
- `/skills help` — print this summary inside the TUI.
- `/<skill-name>` — activate the skill directly.

## Safety

- `skill_view` rejects `..` in `file_path` and verifies the resolved path stays inside the skill directory before reading.
- Files larger than 2 MB are rejected to keep activation payloads bounded.
- Supporting-file listings are capped at the first match per category folder.

## Authoring tips

- Keep the body focused — the whole thing ships to the LLM on every activation.
- If a skill needs reference material, put it in `references/` and instruct the LLM to call `skill_view` to fetch it, rather than pasting it inline.
- Use `platforms` when a skill is OS-specific. Leave it off for portable skills.
- Write instructions in the imperative, second person. The LLM is the audience.

See `examples/skills/plan/SKILL.md` for a starting template.
