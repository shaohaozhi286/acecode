import * as XLSX from 'xlsx';

function cellText(cell) {
  if (!cell) return '';
  if (cell.w != null) return String(cell.w);
  if (cell.f && cell.v == null) return `=${cell.f}`;
  if (cell.v == null) return '';
  if (cell.v instanceof Date) return cell.v.toISOString();
  return String(cell.v);
}

function columnWidthForText(text) {
  const width = Math.min(42, Math.max(10, String(text || '').length + 2));
  return Math.round(width * 7);
}

export function worksheetToXSpreadsheetData(name, worksheet = {}) {
  const ref = worksheet['!ref'];
  const range = ref
    ? XLSX.utils.decode_range(ref)
    : { s: { r: 0, c: 0 }, e: { r: 0, c: 0 } };
  const rows = { len: Math.max(1, range.e.r + 1) };
  const cols = { len: Math.max(1, range.e.c + 1) };
  const maxColWidths = new Map();

  for (let rowIndex = range.s.r; rowIndex <= range.e.r; rowIndex += 1) {
    const row = { cells: {} };
    for (let colIndex = range.s.c; colIndex <= range.e.c; colIndex += 1) {
      const address = XLSX.utils.encode_cell({ r: rowIndex, c: colIndex });
      const text = cellText(worksheet[address]);
      if (!text) continue;
      row.cells[colIndex] = { text };
      maxColWidths.set(
        colIndex,
        Math.max(maxColWidths.get(colIndex) || 0, columnWidthForText(text)),
      );
    }
    if (Object.keys(row.cells).length > 0) {
      rows[rowIndex] = row;
    }
  }

  const declaredCols = Array.isArray(worksheet['!cols']) ? worksheet['!cols'] : [];
  for (let colIndex = 0; colIndex < cols.len; colIndex += 1) {
    const declared = declaredCols[colIndex];
    const declaredWidth = Number(declared?.wpx)
      || (Number(declared?.wch) ? Math.round(Number(declared.wch) * 7) : 0);
    const width = Math.max(declaredWidth, maxColWidths.get(colIndex) || 0);
    if (width > 0) cols[colIndex] = { width };
  }

  const merges = Array.isArray(worksheet['!merges'])
    ? worksheet['!merges'].map((merge) => XLSX.utils.encode_range(merge))
    : [];

  return {
    name: String(name || 'Sheet1'),
    rows,
    cols,
    merges,
  };
}

export function workbookToXSpreadsheetData(workbook) {
  const names = Array.isArray(workbook?.SheetNames) ? workbook.SheetNames : [];
  if (names.length === 0) {
    return [worksheetToXSpreadsheetData('Sheet1', {})];
  }
  return names.map((name) => worksheetToXSpreadsheetData(name, workbook.Sheets?.[name] || {}));
}

export function parseWorkbookArrayBuffer(buffer) {
  const workbook = XLSX.read(buffer, {
    type: 'array',
    cellDates: true,
    cellFormula: true,
    cellHTML: false,
  });
  return workbookToXSpreadsheetData(workbook);
}
