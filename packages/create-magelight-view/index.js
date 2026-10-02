#!/usr/bin/env node
// npm create magelight-view MyMod  →  ./MyMod/ (manifest + React page)
import * as fs from 'node:fs';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const local = args.includes('--local');
const modId = args.find(a => !a.startsWith('--'));
// Same slug rules as the host's ValidSlug: [A-Za-z0-9_.-], not dot-only,
// not dot-ended, not a Windows device name.
const valid = modId && /^[A-Za-z0-9_.-]{1,32}$/.test(modId) && /[A-Za-z0-9]/.test(modId) &&
  !modId.endsWith('.') && !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$)/i.test(modId);
if (!valid) {
  console.error('usage: npm create magelight-view <ModId>   (slug: [A-Za-z0-9_.-], 1..32 chars, not dot-only/dot-ended/a device name)');
  process.exit(1);
}
const dst = path.resolve(process.cwd(), modId);
if (fs.existsSync(dst)) {
  console.error(`${dst} already exists`);
  process.exit(1);
}
const tpl = path.join(here, 'template');
for (const f of fs.readdirSync(tpl, { recursive: true })) {
  const src = path.join(tpl, f);
  if (!fs.statSync(src).isFile()) continue;
  const to = path.join(dst, f.replace(/^_/, '.'));
  fs.mkdirSync(path.dirname(to), { recursive: true });
  fs.writeFileSync(to, fs.readFileSync(src, 'utf8').replaceAll('__MODID__', modId));
}
// --local: point the @magelight/* deps at the cloned packages (file:) so the
// scaffold builds against a clone instead of npm. Requires the packages to be
// built first (npm install && npm run build at the repo root).
if (local) {
  const pkgPath = path.join(dst, 'package.json');
  const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  const rel = { '@magelight/sdk': 'sdk', '@magelight/react': 'react', '@magelight/vite-plugin': 'vite-plugin' };
  for (const grp of ['dependencies', 'devDependencies']) {
    for (const dep of Object.keys(pkg[grp] || {})) {
      if (rel[dep]) pkg[grp][dep] = 'file:' + path.join(here, '..', rel[dep]).split(path.sep).join('/');
    }
  }
  fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n');
  console.log(`created ${modId}/  (--local: @magelight/* point at the cloned packages)
  build the packages ONCE:  (repo root) npm install && npm run build`);
} else {
  console.log(`created ${modId}/`);
}
console.log(`  cd ${modId} && npm install && npm run build   → ${modId}/views/config/index.html
  install: copy manifest.json and views/ to Data/Magelight/${modId}/ (a mod manager mod with Magelight/${modId}/ inside works too)
  in game: F10 opens the page. Before you ship, set "hotkey" in manifest.json to a key no other mod
  and no game control uses. To hot-reload page edits, set "devMode": true in
  My Games/Skyrim Special Edition/SKSE/Magelight.json (Skyrim VR on VR, Skyrim Special Edition GOG on GOG)`);
