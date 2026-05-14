export function assistantChromeState({ showAceCodeAvatar = true, continuation = false } = {}) {
  const enabled = showAceCodeAvatar !== false;
  return {
    showAvatar: enabled && !continuation,
    showName: enabled && !continuation,
    showAvatarPlaceholder: enabled && continuation,
    gapClass: enabled ? 'gap-2' : 'gap-0',
  };
}

export function activityChromeState(showAceCodeAvatar = true) {
  const enabled = showAceCodeAvatar !== false;
  return {
    showAvatar: enabled,
    gapClass: enabled ? 'gap-2' : 'gap-0',
  };
}
