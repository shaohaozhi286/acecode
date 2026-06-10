# Model Context Resolution Memo

## Background

acecode previously treated `context_window` as a mostly static value and used a fallback of `128000`.
That was incorrect for some Copilot models.

Observed example:

- `gpt-5.4` under GitHub Copilot was shown as `128k` in acecode
- Hermes Agent resolved it to `400k`

The practical impact was not limited to display. The wrong value also affected runtime behavior:

- token status bar capacity display
- `/config` output
- auto-compact trigger threshold in `AgentLoop`

## Root Cause

GitHub Copilot's `/models` metadata is not a reliable source for the full model context window.
For some models it exposes provider-side input caps rather than the effective model context used for routing decisions.

That means a naive `GET /models -> parse context -> trust it` strategy can under-report context length.

## Reference From Hermes Agent

Hermes Agent does not blindly trust provider `/models` metadata for known providers.

Its approach is provider-aware:

1. Prefer a curated provider/model metadata source
2. Use endpoint metadata only as a fallback
3. Keep a final hardcoded fallback only for unknown cases

For Copilot, this avoids the common under-reporting problem.

## acecode Solution

acecode now uses a provider-aware runtime resolver implemented in:

- `src/provider/model_context_resolver.cpp`

Current resolution strategy:

1. If provider is `copilot`, prefer `models.dev` provider entry `github-copilot`
2. If provider is official `openai`, prefer `models.dev` provider entry `openai`
3. For generic OpenAI-compatible endpoints, try the endpoint `/models` metadata as fallback
4. If no trustworthy metadata is found, keep the configured fallback value

Session-facing paths use a non-blocking variant of this resolver. When create/resume/model-switch code cannot find a cached or local `models.dev` value, it returns the configured fallback immediately and warms endpoint `/models` metadata in the background for future calls. This keeps small Web sessions from waiting on remote model metadata before the UI can switch sessions.

## Why models.dev Is Preferred For Copilot

For this specific problem, `models.dev` is used as the authoritative provider-aware source because it exposes the model context expected for the provider variant instead of just the raw endpoint limit.

Example:

- `github-copilot / gpt-5.4` resolves to `400000`

This is the value acecode should use for UX and runtime threshold calculations.

## Runtime Integration Points

The resolved value is applied at runtime in two places:

1. App startup
2. `/model <name>` switching
3. Web session create/resume model state

Whenever the model changes, acecode now refreshes:

- `config.context_window`
- `AgentLoop` context limit
- token status display

For Web session create/resume, the first reported `context_window` may be the configured fallback if remote metadata is not cached yet; later session-facing resolutions reuse the process-local cache after a background probe succeeds.

## OpenAI-Compatible Custom Request Headers

OpenAI-compatible saved models can include optional `request_headers` metadata:

```json
{
  "name": "gateway",
  "provider": "openai",
  "base_url": "https://gateway.example.com/v1",
  "api_key": "sk-placeholder",
  "model": "gpt-4o",
  "request_headers": {
    "X-Team": "acecode",
    "X-Token": "{env:ACE_GATEWAY_TOKEN}"
  }
}
```

`request_headers` must be a JSON object with string header names and string template values. Templates may contain `{env:NAME}` placeholders; ACECode resolves them immediately before each OpenAI-compatible chat request or `/models` probe. Keep secrets in environment variables instead of writing literal secrets into `config.json`, because the unresolved templates are returned by the Web/Desktop model editor for later editing.

`Content-Type` is controlled by ACECode and cannot be overridden. A custom `Authorization` header is allowed and overrides the built-in bearer header derived from `api_key`.

For legacy configs, the top-level `openai.request_headers` object is also accepted. It is used as a fallback for OpenAI-compatible saved model entries that do not define their own `request_headers`; per-model headers take precedence.

## Important Rule

For known providers, do not assume `/models` returns the real context window.

Prefer provider-aware metadata first.

## Future Follow-ups

- Cache `models.dev` results locally to avoid repeated startup fetches
- Show resolved context length directly in interactive model selection
- Expand provider mappings if more providers show endpoint metadata drift

## Related Files

- `src/provider/model_context_resolver.cpp`
- `src/provider/model_context_resolver.hpp`
- `src/commands/builtin_commands.cpp`
- `main.cpp`
