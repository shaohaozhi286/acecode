export function parseTimestampMs(value) {
  if (value === undefined || value === null || value === '') return 0;
  if (typeof value === 'number') {
    return Number.isFinite(value) && value > 0 ? value : 0;
  }
  const numeric = Number(value);
  if (Number.isFinite(numeric) && numeric > 0) return numeric;
  const parsed = Date.parse(String(value));
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
}

export function transcriptTimestampMs(item) {
  return parseTimestampMs(item?.ts ?? item?.timestamp_ms ?? item?.timestampMs ?? item?.timestamp);
}
