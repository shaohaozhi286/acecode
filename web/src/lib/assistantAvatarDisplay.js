export function assistantChromeState({ showAceCodeAvatar = true, continuation = false } = {}) {
  const enabled = showAceCodeAvatar !== false;
  return {
    showAvatar: enabled && !continuation,
    showName: enabled && !continuation,
    showAvatarPlaceholder: continuation || !enabled,
    gapClass: 'gap-2',
  };
}

export function activityChromeState(showAceCodeAvatar = true) {
  const enabled = showAceCodeAvatar !== false;
  return {
    showAvatar: enabled,
    showAvatarPlaceholder: !enabled,
    gapClass: 'gap-2',
  };
}
