// Writes the page core's IME block (C++ string literals in src/Magelight.cpp) out as plain JS for ime_box_test.exe.
// Usage: node extract.cjs <Magelight.cpp> <out.js>
const fs = require('fs');
const [, , cppPath, outPath] = process.argv;
const src = fs.readFileSync(cppPath, 'utf8').split(/\r?\n/);
function block(from, to) {
  const start = src.findIndex(l => l.includes(from));
  const end = src.findIndex((l, i) => i > start && l.includes(to));
  if (start < 0 || end < 0) throw new Error('block not found in ' + cppPath + ': ' + from);
  let js = '';
  for (const line of src.slice(start, end)) {
    const t = line.trim();
    if (!t.startsWith('"')) continue;
    js += t.replace(/"\s*\/\/.*$/, '"').replace(/^"/, '').replace(/"$/, '');
  }
  if (!js) throw new Error('empty block in ' + cppPath + ': ' + from);
  return js.replace(/\\\\/g, '\\').replace(/\\"/g, '"');
}
fs.writeFileSync(outPath, block("// IME: the page core draws the IME's candidate list", '// Tooltips (1.2.0): the page draws the title') + '\n');
