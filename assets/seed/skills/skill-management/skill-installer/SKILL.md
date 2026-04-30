---
name: skill-installer
description: Install ACECode skills into ~/.acecode/skills from a local directory, repository checkout, or user-provided skill source.
license: MIT
compatibility: ACECode skill system
metadata:
  source_id: codex-system:skill-installer@2026-04-30
  tags: [skills, installation]
---

# Skill Installer

Use this skill when the user asks to install a skill, copy a skill into ACECode, list installable skill files in a source tree, or move a skill from another agent into ACECode.

## Target Location

Install global user skills under:

```text
~/.acecode/skills/<category>/<skill-name>/SKILL.md
```

ACECode also reads compatible `~/.agent/skills`, but this installer should write to `~/.acecode/skills` unless the user explicitly asks for a different target.

## Installation Workflow

1. Identify the source skill directory. It must contain a `SKILL.md` file.
2. Read the frontmatter and confirm the skill has a usable `name` and `description`.
3. Choose a category. Preserve the source category when obvious; otherwise use `general`.
4. Check whether the target directory already exists.
5. If the target exists, do not overwrite it unless the user explicitly asks.
6. Copy `SKILL.md` and any supporting `references/`, `templates/`, `scripts/`, or `assets/` directories.
7. Tell the user to run `/skills reload` in an existing ACECode session, or restart ACECode, so the registry can rescan disk.

## Safety Rules

- Never silently overwrite a user skill.
- Do not install files outside the chosen skill directory.
- Preserve supporting files only when they are inside the source skill directory.
- Prefer local copies over runtime network dependencies.
- If adapting a skill from another agent, rewrite paths, tool names, and unsupported workflows for ACECode before installing.

## Verification

After installation, verify:

- `SKILL.md` exists at the target.
- Frontmatter still parses.
- The skill directory contains only files intended for the skill.
- A registry reload would discover the skill name.
