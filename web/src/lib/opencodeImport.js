export function normalizeOpencodeImportPreview(preview = {}) {
  const count = Number.isFinite(Number(preview?.count))
    ? Math.max(0, Number(preview.count))
    : 0;
  return {
    ...preview,
    count,
    available: !!preview?.available && count > 0,
  };
}

export function opencodeImportConfirmationText(count) {
  const n = Number.isFinite(Number(count)) ? Math.max(0, Number(count)) : 0;
  return `即将导入 ${n} 个会话，请确认`;
}

export function opencodeImportProgress(status = {}) {
  const total = Number.isFinite(Number(status?.total)) ? Math.max(0, Number(status.total)) : 0;
  const imported = Number.isFinite(Number(status?.imported)) ? Math.max(0, Number(status.imported)) : 0;
  const failed = Number.isFinite(Number(status?.failed)) ? Math.max(0, Number(status.failed)) : 0;
  const skipped = Number.isFinite(Number(status?.skipped)) ? Math.max(0, Number(status.skipped)) : 0;
  const processed = Math.min(total || imported + failed + skipped, imported + failed + skipped);
  const percent = total > 0 ? Math.min(100, Math.round((processed / total) * 100)) : 0;
  return { total, imported, failed, skipped, processed, percent };
}
