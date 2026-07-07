#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT_DIR = path.join(ROOT, 'web', 'public', 'vs-icons');

const ICONPARK_PACKAGE =
  process.env.ICONPARK_SVG_PACKAGE ||
  process.argv.find((arg) => arg.startsWith('--package='))?.slice('--package='.length) ||
  path.join(ROOT, 'node_modules', '@icon-park', 'svg');

const ICON_SETTINGS = Object.freeze({
  theme: 'outline',
  size: 16,
  strokeWidth: 2,
  strokeLinecap: 'round',
  strokeLinejoin: 'round',
  fill: '#333',
});

const ICONS = Object.freeze({
  Add: 'Plus',
  AddFolder: 'FolderPlus',
  Archive: 'InboxIn',
  ArrowLeft: 'ArrowLeft',
  ArrowRight: 'ArrowRight',
  Backwards: 'ArrowLeft',
  Brain: 'Brain',
  Brightness: 'Light',
  Check: 'Check',
  ClearAll: 'ClearAll',
  Close: 'CloseSmall',
  Copy: 'Copy',
  DarkTheme: 'Moon',
  Delete: 'Delete',
  Document: 'FileText',
  Edit: 'Edit',
  EditWindow: 'EditOne',
  Ellipsis: 'MoreFour',
  Embedding: 'BlocksAndArrows',
  Error: 'Error',
  ExpandDown: 'DownSmall',
  ExpandRight: 'RightSmall',
  ExpandUp: 'UpSmall',
  Extension: 'Puzzle',
  Eye: 'Eyes',
  FolderClosed: 'FolderClose',
  FolderOpened: 'FolderOpen',
  Fork: 'Fork',
  GlyphDown: 'DownSmall',
  GlyphUp: 'UpSmall',
  Globe: 'Globe',
  Help: 'Help',
  Info: 'Info',
  LeftBar: 'LeftBar',
  List: 'List',
  Lock: 'Lock',
  NewSession: 'EditOne',
  OpenFile: 'FileSearch',
  PanelLeft: 'LeftBar',
  PanelRight: 'RightBar',
  Pin: 'Pin',
  Refresh: 'Refresh',
  RightBar: 'RightBar',
  Run: 'Play',
  Save: 'Save',
  ScreenFull: 'FullScreen',
  ScreenNormal: 'OffScreen',
  Search: 'Search',
  SearchSparkle: 'Search',
  Settings: 'Setting',
  StatusError: 'Error',
  StatusHelp: 'Help',
  StatusInformation: 'Info',
  StatusNotStarted: 'Radio',
  StatusOK: 'Check',
  StatusRunning: 'LoadingOne',
  StatusWarning: 'Caution',
  Stop: 'Square',
  TerminalReadWrite: 'Terminal',
  Tool: 'Tool',
  WordWrap: 'TextWrapOverflow',
  World: 'World',
});

const CUSTOM_SVGS = Object.freeze({
  ClearAll: `<svg width="16" height="16" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M2.25 2.25L5.25 5.25M5.25 2.25L2.25 5.25" stroke="#333" stroke-width="1.35" stroke-linecap="round"/><path d="M7 3.75H14" stroke="#333" stroke-width="1.35" stroke-linecap="round"/><path d="M2.25 8H14" stroke="#333" stroke-width="1.35" stroke-linecap="round"/><path d="M2.25 12.25H14" stroke="#333" stroke-width="1.35" stroke-linecap="round"/></svg>`,
  NewSession: `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><defs><mask id="ace-icon-new-session-base-cutout" maskUnits="userSpaceOnUse" x="0" y="0" width="24" height="24"><rect width="24" height="24" fill="white"/><path d="M12.2 11.2L19.45 3.95" stroke="black" stroke-width="5.6" stroke-linecap="round"/><path d="M18.6 15.4V21.8M15.4 18.6H21.8" stroke="black" stroke-width="5" stroke-linecap="round"/></mask></defs><g mask="url(#ace-icon-new-session-base-cutout)"><path d="M5.5 3.75H15.25C16.22 3.75 17 4.53 17 5.5V16.5C17 17.47 16.22 18.25 15.25 18.25H5.5C4.53 18.25 3.75 17.47 3.75 16.5V5.5C3.75 4.53 4.53 3.75 5.5 3.75Z" stroke="#333" stroke-width="1.3" stroke-linejoin="round"/><path d="M7.2 8.65H11.7M7.2 11.65H10.35" stroke="#333" stroke-width="1.25" stroke-linecap="round"/></g><path d="M12.2 11.2L19.45 3.95" stroke="#333" stroke-width="1.55" stroke-linecap="round"/><path d="M18.6 15.4V21.8M15.4 18.6H21.8" stroke="#333" stroke-width="1.8" stroke-linecap="round"/></svg>`,
  Refresh: `<svg width="16" height="16" viewBox="0 0 48 48" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M40 20C38.2 12.8 31.7 7.5 24 7.5C18.9 7.5 14.4 9.8 11.3 13.4" stroke="#333" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/><path d="M11 7.5V14H17.5" stroke="#333" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/><path d="M8 28C9.8 35.2 16.3 40.5 24 40.5C29.1 40.5 33.6 38.2 36.7 34.6" stroke="#333" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/><path d="M37 40.5V34H30.5" stroke="#333" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>`,
});

function iconModulePath(iconParkName) {
  return path.join(ICONPARK_PACKAGE, 'lib', 'icons', `${iconParkName}.js`);
}

function loadIcon(iconParkName) {
  const modulePath = iconModulePath(iconParkName);
  if (!fs.existsSync(modulePath)) {
    throw new Error(`IconPark icon not found: ${iconParkName} (${modulePath})`);
  }
  const mod = require(modulePath);
  return mod.default || mod;
}

function normalizeSvg(svg, fileBase) {
  const stableId = `ace-icon-${fileBase.replace(/[^a-z0-9_-]/giu, '-').toLowerCase()}`;
  return String(svg)
    .replace(/^<\?xml[^>]*>\s*/u, '')
    .replace(/id="icon-[^"]*"/gu, `id="${stableId}"`)
    .replace(/url\(#icon-[^)]+\)/gu, `url(#${stableId})`)
    .replace(/href="#icon-[^"]+"/gu, `href="#${stableId}"`)
    .replace(/\r\n/g, '\n')
    .trim() + '\n';
}

function main() {
  if (!fs.existsSync(ICONPARK_PACKAGE)) {
    throw new Error(
      `IconPark package not found: ${ICONPARK_PACKAGE}\n` +
      'Set ICONPARK_SVG_PACKAGE to an extracted @icon-park/svg package root.',
    );
  }

  fs.mkdirSync(OUT_DIR, { recursive: true });
  for (const [fileBase, iconParkName] of Object.entries(ICONS)) {
    const svg = CUSTOM_SVGS[fileBase]
      ? normalizeSvg(CUSTOM_SVGS[fileBase], fileBase)
      : normalizeSvg(loadIcon(iconParkName)(ICON_SETTINGS), fileBase);
    fs.writeFileSync(path.join(OUT_DIR, `${fileBase}.svg`), svg, 'utf8');
  }

  console.log(`Generated ${Object.keys(ICONS).length} IconPark SVG files in ${OUT_DIR}`);
}

main();
