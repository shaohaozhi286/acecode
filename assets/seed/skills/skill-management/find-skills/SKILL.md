---
name: find-skills
description: Discover installed ACECode skills that match a user task and identify when a new skill should be installed or created.
license: MIT
compatibility: ACECode skill system
metadata:
  source_id: claude-code-haha:find-skills@76d21ddf33ef7927294cdc019b83b6d263a19ac6
  tags: [skills, discovery]
---

# Find Skills

Use this skill when the user asks whether a skill exists, asks how to do a task that may have a skill, or wants to extend ACECode with additional capabilities.

## Workflow

1. List installed skills with `skills_list`.
2. Compare skill names, descriptions, and categories against the user's task.
3. If a promising skill exists, load it with `skill_view` before giving instructions or acting.
4. If no installed skill fits, explain that no matching local skill is installed and suggest either installing one with `skill-installer` or creating one with `skill-creator`.
5. Keep recommendations scoped to skills. Do not invent installed skills that are not returned by `skills_list`.

## Matching Guidelines

- Prefer exact skill descriptions over category guesses.
- Treat project-local skills as normal installed skills; the registry has already applied ACECode's precedence rules.
- If two skills overlap, pick the one with the narrowest description for the current task.
- If the user asks for a skill by name, search by that name first, then by related capability terms.

## Output

When reporting results, include:

- The matching skill name.
- Why it matches.
- Whether you loaded it with `skill_view`.
- The next action: use it, install a missing skill, or create a new skill.
