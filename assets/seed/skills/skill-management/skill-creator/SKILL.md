---
name: skill-creator
description: Create or update ACECode SKILL.md files with focused instructions, metadata, and optional supporting files.
license: MIT
compatibility: ACECode skill system
metadata:
  source_id: codex-system:skill-creator@2026-04-30
  tags: [skills, authoring]
---

# Skill Creator

Use this skill when the user wants to create a new skill or improve an existing skill for ACECode.

## When to Create a Skill

Create a skill when the workflow is reusable, benefits from stable instructions, or needs domain-specific steps that should not be repeated in every prompt.

Do not create a skill for one-off context, secrets, large generated output, or behavior that should live in project code instead.

## Skill Layout

Use this layout for global skills:

```text
~/.acecode/skills/<category>/<skill-name>/
  SKILL.md
  references/   # optional
  templates/    # optional
  scripts/      # optional
  assets/       # optional
```

Project-specific skills can live under `<project>/.acecode/skills/` with the same structure.

## Required Frontmatter

```markdown
---
name: example-skill
description: Clear one-line trigger description for when ACECode should use this skill.
license: MIT
compatibility: ACECode skill system
metadata:
  tags: [example]
---
```

Keep `name` stable and kebab-case friendly. Keep `description` specific enough that `skills_list` can help the model choose correctly.

## Body Guidelines

- Write direct instructions to the model.
- Start with when to use the skill.
- Include a short workflow with concrete steps.
- Put large reference material in `references/` and tell the model to load it with `skill_view`.
- Put reusable snippets in `templates/`.
- Put executable helpers in `scripts/` only when they are safer than retyping commands.
- Avoid unsupported assumptions such as subagents, unavailable tools, or automatic network access.

## Update Workflow

1. Read any existing `SKILL.md`.
2. Preserve working metadata and user-authored content unless a change is requested.
3. Tighten the trigger description before adding more body text.
4. Validate that the skill can be understood from `skills_list` metadata alone.
5. Tell the user to run `/skills reload` after editing an installed skill.
