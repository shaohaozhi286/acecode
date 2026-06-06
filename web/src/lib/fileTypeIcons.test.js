import { strict as assert } from 'node:assert';
import { fileTypeIconForPath } from './fileTypeIcons.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

function assertIcon(path, expected) {
  assert.equal(fileTypeIconForPath(path).id, expected);
}

run('fileTypeIconForPath maps language contribution extensions', () => {
  assertIcon('docs/spec.md', '_markdown');
  assertIcon('index.js', '_javascript');
  assertIcon('web/src/App.jsx', '_react');
  assertIcon('types.ts', '_typescript');
  assertIcon('web/src/App.tsx', '_react');
  assertIcon('main.py', '_python');
  assertIcon('lib.rs', '_rust');
  assertIcon('main.go', '_go2');
});

run('fileTypeIconForPath maps Seti theme extension associations', () => {
  assertIcon('styles/globals.css', '_css');
  assertIcon('styles/theme.scss', '_sass');
  assertIcon('styles/theme.sass', '_sass');
  assertIcon('styles/theme.less', '_less');
  assertIcon('compile_commands.json', '_json');
  assertIcon('web/src/Component.vue', '_vue');
});

run('fileTypeIconForPath prefers exact filename associations', () => {
  assertIcon('README.md', '_info');
  assertIcon('LICENSE', '_license');
  assertIcon('.gitignore', '_git');
});

run('fileTypeIconForPath uses compound extension associations before short extensions', () => {
  assertIcon('assets/site.css.map', '_css');
  assertIcon('src/foo.spec.js', '_javascript_1');
  assertIcon('src/foo.test.ts', '_typescript_1');
});

run('fileTypeIconForPath uses VS Code language filename patterns', () => {
  assertIcon('Dockerfile.dev', '_docker');
  assertIcon('Containerfile.local', '_docker');
  assertIcon('.env.local', '_config');
  assertIcon('compose.dev.yml', '_docker_3');
  assertIcon('.github/hooks/pre-commit.json', '_json');
});

run('fileTypeIconForPath returns the Seti default file icon for unknown files', () => {
  assertIcon('unknown.nope', '_default');
  assertIcon('', '_default');
});
