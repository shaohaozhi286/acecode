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
  const match = /^\s*\/(btw|side)\b([\s\S]*)$/i.exec(value);
  if (!match) return null;
  return {
    command: match[1].toLowerCase(),
    question: match[2].trim(),
    display_text: value.trim(),
  };
}

export function turnSteerRequestForText(text) {
  const value = String(text || '');
  const match = /^\s*\/turn\b([\s\S]*)$/i.exec(value);
  if (!match) return null;
  return {
    guidance: match[1].trim(),
    display_text: value.trim(),
  };
}

export function inputRouteForText(text) {
  const sideQuestion = sideQuestionRequestForText(text);
  if (sideQuestion) return { kind: 'side_question', ...sideQuestion };
  const turnSteer = turnSteerRequestForText(text);
  if (turnSteer) return { kind: 'turn_steer', ...turnSteer };
  const command = builtinCommandRequestForText(text);
  if (command) return { kind: 'builtin', command };
  return { kind: 'message', text };
}

export function sessionCreateOptionsForText(text) {
  if (sideQuestionRequestForText(text) || turnSteerRequestForText(text)) {
    return { auto_start: false };
  }
  const command = builtinCommandRequestForText(text);
  if (command) return { auto_start: false };
  return {
    initial_user_message: text,
    auto_start: true,
  };
}
