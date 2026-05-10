import { parseExecutableBuiltinCommand } from './slashCommands.js';

export function builtinCommandRequestForText(text) {
  const builtin = parseExecutableBuiltinCommand(text);
  if (!builtin) return null;
  return {
    command: builtin.name,
    args: builtin.args,
    display_text: builtin.display_text,
  };
}

export function inputRouteForText(text) {
  const command = builtinCommandRequestForText(text);
  if (command) return { kind: 'builtin', command };
  return { kind: 'message', text };
}

export function sessionCreateOptionsForText(text) {
  const command = builtinCommandRequestForText(text);
  if (command) return { auto_start: false };
  return {
    initial_user_message: text,
    auto_start: true,
  };
}
