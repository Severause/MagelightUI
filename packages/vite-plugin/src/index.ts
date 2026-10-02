import * as fs from 'node:fs';
import * as path from 'node:path';
import type { Plugin, UserConfig } from 'vite';

export interface MagelightPluginOptions {
  /**
   * Entry pages, view name → html file (relative to the project root).
   * Each becomes its own pruned folder `<viewsDir>/<name>/index.html` holding
   * only the assets that entry reaches. Omit for a single-entry project
   * (Vite's own `index.html` → `<viewsDir>/<single>`, default `index`).
   */
  entries?: Record<string, string>;
  /** where the per-view folders go, relative to the project root (default `views`) */
  viewsDir?: string;
  /** name for the single-entry case (default `index`) */
  single?: string;
  /** Vite's intermediate outDir (default `dist`) — pruned copies are made from it */
  outDir?: string;
  /** leave `dist` in place after the views are written (default false) */
  keepDist?: boolean;
}

/**
 * What every Magelight page needs from Vite, learned on SeverActions:
 *  - `base: './'` — pages load from file:///, absolute asset URLs break
 *  - `crossorigin` stripped from module tags — a null origin refuses CORS-mode loads (blank page)
 *  - es2022 output — Ultralight 1.4 (WebKit 615) evaluates it; es2019 was the 1.3-era rule
 *  - multi-entry builds emit ONE dist; each view folder must carry only its own closure
 *    (SeverActions' build-ui.ps1 did this by hand; here it is the plugin's job)
 */
export function magelight(opts: MagelightPluginOptions = {}): Plugin {
  const viewsDir = opts.viewsDir ?? 'views';
  const outDir = opts.outDir ?? 'dist';
  let root = process.cwd();
  // chunk fileName → the files it needs (imports + css + assets), filled in generateBundle
  const closure = new Map<string, Set<string>>();
  const entryFile = new Map<string, string>(); // view name → html fileName in the bundle

  return {
    name: 'magelight',
    config(): UserConfig {
      const input: Record<string, string> = {};
      if (opts.entries) for (const [k, v] of Object.entries(opts.entries)) input[k] = path.resolve(root, v);
      return {
        base: './',
        build: {
          outDir,
          assetsDir: 'assets',
          target: 'es2022',
          sourcemap: false,
          chunkSizeWarningLimit: 4000,
          modulePreload: { polyfill: false },
          rollupOptions: opts.entries ? { input, output: { manualChunks: undefined } } : { output: { manualChunks: undefined } },
        },
      };
    },
    configResolved(cfg) {
      root = cfg.root;
    },
    transformIndexHtml(html) {
      return html.replace(/\scrossorigin(="[^"]*")?/g, '');
    },
    generateBundle(_, bundle) {
      // Chunks and assets are here; the HTML entries are emitted by Vite's
      // own html plugin AFTER this hook, so they are discovered on disk in
      // closeBundle instead.
      for (const [file, out] of Object.entries(bundle)) {
        if (out.type !== 'chunk') continue;
        const deps = new Set<string>();
        for (const i of out.imports) deps.add(i);
        for (const i of out.dynamicImports) deps.add(i);
        const meta = (out as unknown as { viteMetadata?: { importedCss?: Set<string>; importedAssets?: Set<string> } }).viteMetadata;
        meta?.importedCss?.forEach((c) => deps.add(c));
        meta?.importedAssets?.forEach((a) => deps.add(a));
        closure.set(file, deps);
      }
    },
    closeBundle() {
      const dist = path.resolve(root, outDir);
      const views = path.resolve(root, viewsDir);
      if (!fs.existsSync(dist)) return;
      // Entry html files as Vite wrote them (relative to dist), mapped to view names.
      const htmls = (fs.readdirSync(dist, { recursive: true }) as string[])
        .filter((f) => f.endsWith('.html'))
        .map((f) => f.split(path.sep).join('/'));
      const norm = (s: string) => s.split(path.sep).join('/').replace(/^\.\//, '');
      for (const html of htmls) {
        let name: string | undefined;
        if (opts.entries) {
          const exact = Object.entries(opts.entries).filter(([, v]) => norm(v) === html);
          if (exact.length) {
            name = exact[0][0];
          } else {
            // Vite flattens some inputs — fall back to the file name, but only
            // when ONE entry carries it: pages/a/index.html + pages/b/index.html
            // must not both claim the first match.
            const byBase = Object.entries(opts.entries).filter(([, v]) => path.posix.basename(norm(v)) === path.posix.basename(html));
            if (byBase.length === 1) name = byBase[0][0];
            else if (byBase.length > 1)
              console.warn(`[magelight] ${html}: ambiguous — entries ${byBase.map(([k]) => k).join(', ')} share a file name; skipped`);
          }
        } else {
          if (htmls.length > 1 && html !== 'index.html') {
            console.warn(`[magelight] ${html}: single-entry mode emits one view ('${opts.single ?? 'index'}'); extra html skipped — use 'entries' for a multi-view project`);
            continue;
          }
          name = opts.single ?? 'index';
        }
        if (name) entryFile.set(name, html);
      }
      let total = 0;
      for (const [name, htmlFile] of entryFile) {
        const src = fs.readFileSync(path.join(dist, htmlFile), 'utf8');
        const htmlDir = path.posix.dirname(htmlFile);
        const roots = new Set<string>();
        for (const m of src.matchAll(/(?:src|href)="\.\/([^"]+)"/g)) roots.add(path.posix.normalize(path.posix.join(htmlDir, m[1])));
        const needed = new Set<string>();
        const walk = (f: string) => {
          if (needed.has(f)) return;
          needed.add(f);
          closure.get(f)?.forEach(walk);
        };
        roots.forEach(walk);
        const dst = path.join(views, name);
        fs.rmSync(dst, { recursive: true, force: true });
        fs.mkdirSync(dst, { recursive: true });
        fs.writeFileSync(path.join(dst, 'index.html'), src.replace(/(src|href)="\.\/([^"]+)"/g, (_m, a, rel) => `${a}="./${path.posix.relative('', path.posix.normalize(path.posix.join(htmlDir, rel)))}"`));
        for (const f of needed) {
          const from = path.join(dist, f);
          if (!fs.existsSync(from)) continue;
          const to = path.join(dst, f);
          fs.mkdirSync(path.dirname(to), { recursive: true });
          fs.copyFileSync(from, to);
          total++;
        }
        // public/ files (copied verbatim by Vite) are not in the graph — carry them all.
        const pub = path.resolve(root, 'public');
        if (fs.existsSync(pub)) {
          for (const f of fs.readdirSync(pub, { recursive: true }) as string[]) {
            const from = path.join(pub, f);
            if (fs.statSync(from).isFile()) {
              const to = path.join(dst, f);
              fs.mkdirSync(path.dirname(to), { recursive: true });
              fs.copyFileSync(from, to);
            }
          }
        }
        console.log(`[magelight] view '${name}' -> ${path.relative(root, dst)} (${needed.size + 1} files)`);
      }
      if (!opts.keepDist) fs.rmSync(dist, { recursive: true, force: true });
      if (!total) console.warn('[magelight] no entries were written — check `entries` names match the html files');
    },
  };
}

export default magelight;
