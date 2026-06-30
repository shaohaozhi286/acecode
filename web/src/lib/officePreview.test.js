import assert from 'node:assert/strict';
import * as XLSX from 'xlsx';
import {
  parseWorkbookArrayBuffer,
  workbookToXSpreadsheetData,
  worksheetToXSpreadsheetData,
} from './officePreview.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

async function runAsync(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

run('worksheetToXSpreadsheetData maps cells, columns, and merges', () => {
  const worksheet = {
    '!ref': 'A1:C3',
    A1: { v: 'Name' },
    B1: { v: 'Score' },
    A2: { v: 'Ada' },
    B2: { v: 42, w: '42' },
    C3: { f: 'SUM(B2:B2)' },
    '!cols': [{ wch: 18 }],
    '!merges': [{ s: { r: 0, c: 0 }, e: { r: 0, c: 1 } }],
  };

  const sheet = worksheetToXSpreadsheetData('Results', worksheet);

  assert.equal(sheet.name, 'Results');
  assert.equal(sheet.rows.len, 3);
  assert.equal(sheet.cols.len, 3);
  assert.equal(sheet.rows[0].cells[0].text, 'Name');
  assert.equal(sheet.rows[1].cells[1].text, '42');
  assert.equal(sheet.rows[2].cells[2].text, '=SUM(B2:B2)');
  assert.equal(sheet.merges[0], 'A1:B1');
  assert.equal(sheet.cols[0].width >= 126, true);
});

run('workbookToXSpreadsheetData preserves sheet order and empty workbooks', () => {
  const workbook = {
    SheetNames: ['First', 'Second'],
    Sheets: {
      First: { '!ref': 'A1:A1', A1: { v: 'a' } },
      Second: { '!ref': 'A1:A1', A1: { v: 'b' } },
    },
  };

  assert.deepEqual(
    workbookToXSpreadsheetData(workbook).map((sheet) => sheet.name),
    ['First', 'Second'],
  );
  assert.equal(workbookToXSpreadsheetData({}).length, 1);
});

await runAsync('parseWorkbookArrayBuffer parses xlsx buffers for preview', async () => {
  const workbook = XLSX.utils.book_new();
  const worksheet = XLSX.utils.aoa_to_sheet([
    ['Name', 'Score'],
    ['Ada', 42],
  ]);
  XLSX.utils.book_append_sheet(workbook, worksheet, 'Scores');
  const buffer = XLSX.write(workbook, { type: 'array', bookType: 'xlsx' });

  const data = parseWorkbookArrayBuffer(buffer);

  assert.equal(data[0].name, 'Scores');
  assert.equal(data[0].rows[0].cells[0].text, 'Name');
  assert.equal(data[0].rows[1].cells[1].text, '42');
});
