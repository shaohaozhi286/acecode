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

export function sideQuestionRequestForText(text) {
  const value = String(text || '');
  const match = /^\s*\/btw\b([\s\S]*)$/i.exec(value);
  if (!match) return null;
  return {
    question: match[1].trim(),
    display_text: value.trim(),
  };
}

export function inputRouteForText(text) {
  const sideQuestion = sideQuestionRequestForText(text);
  if (sideQuestion) return { kind: 'side_question', ...sideQuestion };
  const command = builtinCommandRequestForText(text);
  if (command) return { kind: 'builtin', command };
  return { kind: 'message', text };
}

export function sessionCreateOptionsForText(text) {
  if (sideQuestionRequestForText(text)) return { auto_start: false };
  const command = builtinCommandRequestForText(text);
  if (command) return { auto_start: false };
  return {
    initial_user_message: text,
    auto_start: true,
  };
}
