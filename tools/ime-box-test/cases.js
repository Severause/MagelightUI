// Runs before the bridge's text-focus and IME blocks, with a stub window.magelight: each case focuses a field,
// sends '__ime' payloads and returns what it measured. Coordinates are getBoundingClientRect's, all in the same
// space. A case that sets window.__imeTypeText has those characters typed by the host (real key events), then
// __imeAfterType() finishes it.
(function () {
  var subs = {};
  window.__mlNative = function () {};
  window.magelight = { on: function (ch, fn) { (subs[ch] = subs[ch] || []).push(fn); } };
  window.__imeSend = function (m) {
    var full = { state: 'candidates', text: '', composition: '', caret: 0, candidates: [], selectedIndex: -1 };
    for (var k in m) full[k] = m[k];
    (subs.__ime || []).forEach(function (fn) { fn(JSON.stringify(full)); });
  };
  var fr = document.getElementById('fr');
  var fd = fr.contentDocument;
  fd.open();
  fd.write('<!doctype html><html><body style="margin:0"><div id="cm" contenteditable="true" ' +
           'style="font:18px monospace;padding:12px;outline:none">{{ npc.name }} greets the player.<br>' +
           'Second line of the prompt</div></body></html>');
  fd.close();
  function rect(e) { var r = e.getBoundingClientRect(); return { l: r.left, t: r.top, r: r.right, b: r.bottom, w: r.width, h: r.height }; }
  var nine = ['東京', 'とうきょう', 'トウキョウ', '東京都', '凍狂', '東京駅', '東京タワー', '東響', '登京'];
  // The window's bottom-right corner in the same units as the box, measured apart from the page core's own marker.
  function corner() {
    var p = document.createElement('span');
    p.style.cssText = 'position:fixed;right:0;bottom:0;width:0;height:0';
    document.body.appendChild(p);
    var r = p.getBoundingClientRect();
    document.body.removeChild(p);
    return { r: r.left, b: r.top };
  }
  var f0 = document.getElementById('f');
  window.__imeAfterType = function () {
    __imeSend({ state: 'commit', text: 'とうきょう' });
    __imeSend({ state: 'end' });
    var out = { n: 8, value: f0.value, maxLength: f0.maxLength, ended: window.__imeEnded || null };
    var fail = [];
    if (f0.value !== 'abとうきょう') fail.push('the commit did not replace the reading within maxlength (' + f0.value + ')');
    if (window.__imeEnded !== 'とうきょう') fail.push('compositionend did not carry the commit');
    out.ok = fail.length === 0;
    out.fail = fail;
    window.__imeTypeText = '';
    return JSON.stringify(out);
  };
  window.__imeCase = function (n) {
    var zoom = n === 0 || n === 7 || n === 8 ? '' : '1.13';
    document.body.style.zoom = zoom;
    document.documentElement.style.zoom = n === 7 ? '1.25' : '';
    var out = { n: n, zoom: zoom || '1', htmlZoom: n === 7 ? '1.25' : '1', innerWidth: innerWidth, innerHeight: innerHeight };
    if (n === 8) {
      // A field with a maxlength and the IME's caret inside the reading: the commit, typed as real keys, must still
      // replace the whole reading and fit.
      f0.style.left = ''; f0.style.width = '';
      f0.value = 'ab';
      f0.maxLength = 7;
      f0.focus();
      f0.setSelectionRange(2, 2);
      f0.addEventListener('compositionend', function (ev) { window.__imeEnded = ev.data; });
      __imeSend({ state: 'start' });
      __imeSend({ state: 'compose', text: 'とうきょう', composition: 'とうきょう', caret: 3, clauses: [0, 5] });
      window.__imeTypeText = 'とうきょう';
      return JSON.stringify({ n: 8, pending: true });
    }
    if (n === 4 || n === 5) {
      // CodeMirror's caret at the end of the document, collapsed at an element boundary (WebKit reports 0x0 for
      // it): case 4 on an empty last line, case 5 after text.
      var cm2 = fd.getElementById('cm');
      cm2.innerHTML = n === 4 ? '<div class="cm-line">{{ "# Setting" }}</div><div class="cm-line"><br></div>'
                              : '<div class="cm-line">{{ "# Setting" }}</div><div class="cm-line">abc</div>';
      fr.contentWindow.focus();
      cm2.focus();
      var end = fd.createRange();
      end.selectNodeContents(cm2);
      end.collapse(false);
      var s2 = fr.contentWindow.getSelection();
      s2.removeAllRanges();
      s2.addRange(end);
      var last = cm2.lastChild, lr = last.getBoundingClientRect();
      if (n === 5) {
        var t = fd.createRange();
        t.setStart(last.firstChild, 2);
        t.setEnd(last.firstChild, 3);
        var tr = t.getBoundingClientRect();
        out.caretInFrame = { l: tr.right, t: tr.top, b: tr.bottom };
      } else {
        out.caretInFrame = { l: lr.left, t: lr.top, b: lr.bottom };
      }
      out.frame = rect(fr);
      out.frameInner = fr.contentWindow.innerWidth;
      __imeSend({ state: 'candidates', composition: 'とうきょう', candidates: nine, selectedIndex: 0 });
    } else if (n === 2) {
      var cm = fd.getElementById('cm');
      fr.contentWindow.focus();
      cm.focus();
      var range = fd.createRange();
      range.setStart(cm.firstChild, 12);
      range.collapse(true);
      var s = fr.contentWindow.getSelection();
      s.removeAllRanges();
      s.addRange(range);
      var rr = range.getBoundingClientRect(), fr0 = fr.getBoundingClientRect();
      out.caretInFrame = { l: rr.left, t: rr.top, b: rr.bottom };
      out.frame = rect(fr);
      out.frameInner = fr.contentWindow.innerWidth;
      __imeSend({ state: 'candidates', composition: 'とうきょう', candidates: nine, selectedIndex: 2 });
    } else if (n === 6) {
      // An inline reading converted into three clauses, the second the target: the field selects it and the box
      // anchors at its start, as the Windows IME's window does.
      var f6 = document.getElementById('f');
      f6.value = 'こんにちは、';
      f6.focus();
      f6.setSelectionRange(6, 6);
      var s6 = '今日は良い天気';
      __imeSend({ state: 'start' });
      __imeSend({ state: 'compose', text: s6, composition: s6, caret: 7, clauses: [0, 3, 5, 7], target: 1 });
      __imeSend({ state: 'candidates', composition: s6, caret: 7, clauses: [0, 3, 5, 7], target: 1, candidates: ['良い', 'いい', '善い'], selectedIndex: 0 });
      out.field = rect(f6);
      out.sel = [f6.selectionStart, f6.selectionEnd];
      var cs6 = getComputedStyle(f6), cv6 = document.createElement('canvas').getContext('2d');
      cv6.font = cs6.font;
      out.clauseX = out.field.l + parseFloat(cs6.paddingLeft) + parseFloat(cs6.borderLeftWidth) +
                    cv6.measureText(f6.value.slice(0, 9)).width - f6.scrollLeft;
    } else if (n === 7) {
      // A zoom on <html>, not <body>, and the field near the window's right edge: the box stays in the window.
      f0.style.left = '1400px';
      f0.style.width = '120px';
      f0.focus();
      f0.setSelectionRange(f0.value.length, f0.value.length);
      out.field = rect(f0);
      out.corner = corner();
      __imeSend({ state: 'candidates', composition: '東京', candidates: nine, selectedIndex: 0 });
    } else {
      var f = document.getElementById('f');
      f.style.left = ''; f.style.width = '';
      f.focus();
      f.setSelectionRange(f.value.length, f.value.length);
      out.field = rect(f);
      var c = n === 3 ? ['とても長い予測変換の候補がここに表示されますとても長い予測変換の候補がここに表示されます', '東京'] : nine;
      __imeSend({ state: 'candidates', composition: '東京', candidates: c, selectedIndex: 0 });
    }
    var box = document.getElementById('__ml_ime');
    out.active = (document.activeElement && document.activeElement.id) || '';
    out.box = box && box.style.display === 'block' ? rect(box) : null;
    out.boxCss = box ? { left: box.style.left, top: box.style.top, parent: box.parentNode && box.parentNode.tagName } : null;
    // The checks, in getBoundingClientRect's units, which the box and the fields share (all inside <body>).
    var b = out.box, fail = [];
    if (!b) fail.push('no box');
    else if (n === 7) {
      if (b.l < 0 || b.t < 0 || b.r > out.corner.r + 1 || b.b > out.corner.b + 1)
        fail.push('the box leaves the window (' + b.r.toFixed(0) + ',' + b.b.toFixed(0) + ' vs ' + out.corner.r.toFixed(0) + ',' + out.corner.b.toFixed(0) + ')');
      if (out.field.t - b.b < 0 || out.field.t - b.b > 6) fail.push('not just above the field');
    } else if (out.field) {
      var gap = out.field.t - b.b;
      if (!(gap >= 0 && gap <= 4)) fail.push('not just above the field (gap ' + gap.toFixed(1) + ')');
      if (!(b.l >= out.field.l && b.l < out.field.r)) fail.push('left edge outside the field');
      if (n === 3 && b.w > 481) fail.push('wider than --ml-ime-max-width (' + b.w.toFixed(0) + ')');
      if (n === 6) {
        if (out.sel[0] !== 9 || out.sel[1] !== 11) fail.push('the field does not select the target clause (' + out.sel + ')');
        if (Math.abs(b.l - out.clauseX) > 4) fail.push('not at the target clause x (' + b.l.toFixed(1) + ' vs ' + out.clauseX.toFixed(1) + ')');
      }
    } else {
      var s0 = out.frameInner ? out.frame.w / out.frameInner : 1;
      var caretBottom = out.frame.t + s0 * out.caretInFrame.b, caretLeft = out.frame.l + s0 * out.caretInFrame.l;
      if (!(b.t >= caretBottom && b.t - caretBottom <= 6)) fail.push('not just below the caret');
      if (Math.abs(b.l - caretLeft) > 4) fail.push('not at the caret x (' + b.l.toFixed(1) + ' vs ' + caretLeft.toFixed(1) + ')');
    }
    if (box && box.parentNode !== document.body) fail.push('not in <body>');
    out.ok = fail.length === 0;
    out.fail = fail;
    return JSON.stringify(out);
  };
})();
