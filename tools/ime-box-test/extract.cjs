// Writes the bridge's text-focus and IME blocks (C++ string literals in src/Magelight.cpp) out as plain JS, view
// id 7, for ime_box_test.exe. Usage: node extract.cjs <Magelight.cpp> <out.js>
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
    const lit = t.replace(/"\s*\/\/.*$/, '"').replace(/^"/, '').replace(/"$/, '');
    js += lit.replace(/" \+ idStr0 \+ "/g, '7');
  }
  return js.replace(/\\\\/g, '\\').replace(/\\"/g, '"');
}
fs.writeFileSync(outPath, block("// Text-field focus -> the reserved '__textfocus' channel", '// UI sounds (0.29.0)') + '\n' +
                          block("// IME: the page core draws what the host hands it on '__ime'", '// Tooltips (1.2.0): the page draws the title') + '\n');
