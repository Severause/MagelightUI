// Runs before the page core's IME block, with a stub window.magelight. __imeCase(n) sets a case up and returns what
// the IME holds (ime_box_test.exe composes it with the view's Editor, reads the anchor as the host does and sends the
// '__ime' payload through __imeSend); __imeCheck(n) then measures the box against the page's own geometry, in
// getBoundingClientRect's units, and returns the verdict.
(function () {
  var subs = {}, events = [], cancelNext = false;
  window.__mlNative = function () {};
  window.magelight = { on: function (ch, fn) { (subs[ch] = subs[ch] || []).push(fn); } };
  window.__imeSend = function (json) { (subs.__ime || []).forEach(function (fn) { fn(json); }); };
  window.addEventListener('magelight:ime', function (e) {
    events.push(e.detail);
    if (cancelNext) e.preventDefault();
  });
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
  // Where character `index` of a single-line input starts, in its document's getBoundingClientRect units.
  function inputX(f, index) {
    var cs = getComputedStyle(f), cv = document.createElement('canvas').getContext('2d');
    cv.font = cs.font;
    return rect(f).l + parseFloat(cs.paddingLeft) + parseFloat(cs.borderLeftWidth) +
           cv.measureText(f.value.slice(0, index)).width - f.scrollLeft;
  }
  // The rect of the first character of `text` in document `d`, in d's own units, or null.
  function charRect(d, text) {
    var w = d.createTreeWalker(d.body, NodeFilter.SHOW_TEXT), t;
    while ((t = w.nextNode())) {
      var i = t.data.indexOf(text);
      if (i < 0) continue;
      var g = d.createRange();
      g.setStart(t, i);
      g.setEnd(t, i + 1);
      return g.getBoundingClientRect();
    }
    return null;
  }
  var f0 = document.getElementById('f');
  // A chat bar over the game: a container holding a backdrop layer (a gradient of
  // translucent browns, absolute, z-index 0, pointer-events none) and, in front of it at z-index 1, the input line
  // with a transparent input; the page has no ground of its own, the game is behind it. Not one of the field's
  // ancestors carries the brown. A vignette (a pointer-events none overlay over everything) can lie in front.
  var chat = document.createElement('div');
  chat.id = 'chat';
  chat.style.cssText = 'position:fixed;left:0;right:0;bottom:0;height:300px;padding:0;display:none';
  var backdrop = document.createElement('div');
  backdrop.className = 'chat-backdrop';
  backdrop.style.cssText = 'position:absolute;inset:0;z-index:0;padding:0;pointer-events:none;display:none;' +
                           '-webkit-backdrop-filter:blur(8px);backdrop-filter:blur(8px);' +
                           'background:linear-gradient(to top, rgba(10,8,5,0.92), rgba(10,8,5,0.4))';
  var line = document.createElement('div');
  line.className = 'chat-input-line';
  line.style.cssText = 'position:relative;z-index:1;display:block;padding:8px;background:transparent';
  chat.appendChild(backdrop);
  chat.appendChild(line);
  document.body.appendChild(chat);
  var vignette = document.createElement('div');
  vignette.style.cssText = 'position:fixed;inset:0;padding:0;z-index:100;pointer-events:none;display:none;background:rgba(255,0,0,0.1)';
  document.body.appendChild(vignette);
  var cur = {};
  function field(value, left) {
    f0.removeAttribute('data-ml-ime');
    f0.style.left = left || '';
    f0.style.right = '';
    f0.style.width = '';
    f0.style.font = '';
    f0.style.zoom = '';
    f0.style.background = '';
    f0.style.color = '';
    f0.style.border = '';
    f0.value = value;
    f0.focus();
    f0.setSelectionRange(value.length, value.length);
    return value.length;
  }
  function ime(o) {
    var full = { text: '', caret: 0, clauses: [], target: -1, candidates: nine, selectedIndex: 0 };
    for (var k in o) full[k] = o[k];
    return JSON.stringify(full);
  }
  window.__imeCase = function (n) {
    events.length = 0;
    cancelNext = n === 9;
    var zoom = [1, 2, 3, 4, 5, 12, 13].indexOf(n) >= 0 ? '1.13' : '';
    document.body.style.zoom = zoom;
    document.documentElement.style.removeProperty('--ml-ime-font');
    fd.getElementById('cm').style.fontSize = '18px';
    // The colour cases' page: no --ml-* colours, the page's own grounds, the editor's own colours.
    ['bg', 'fg', 'border', 'selected-bg', 'selected-fg'].forEach(function (k) {
      document.documentElement.style.removeProperty('--ml-ime-' + k);
      document.documentElement.style.removeProperty('--ml-tooltip-' + k);
    });
    document.documentElement.style.background = '';
    document.body.style.background = '';
    chat.style.display = 'none';
    chat.style.background = '';
    backdrop.style.display = 'none';
    vignette.style.display = 'none';
    if (f0.parentNode !== document.body) document.body.appendChild(f0);
    fd.body.style.background = '';
    fd.getElementById('cm').style.color = '';
    document.documentElement.style.zoom = n === 7 ? '1.25' : '';
    cur = { n: n, zoom: zoom || '1', htmlZoom: n === 7 ? '1.25' : '1', innerWidth: innerWidth, innerHeight: innerHeight };
    if (n === 2 || n === 4 || n === 5 || n === 12 || n === 19) {
      // An editor in a same-origin iframe: n 2 mid-line, 4 CodeMirror's caret on an empty last line, 5 at the end of
      // a last line of text (collapsed at an element boundary, where WebKit reports a 0x0 range rect).
      var cm = fd.getElementById('cm');
      if (n === 12) cm.style.fontSize = '26px';   // the editor's own size, in a frame under a zoom on <body>
      if (n === 19) {   // an editor with its own colours, in a frame with its own ground
        cm.style.color = '#d0f0d0';
        fd.body.style.background = '#102010';
      }
      if (n === 2 || n === 12 || n === 19) cm.innerHTML = '{{ npc.name }} greets the player.<br>Second line of the prompt';
      else cm.innerHTML = n === 4 ? '<div class="cm-line">{{ "# Setting" }}</div><div class="cm-line"><br></div>'
                                  : '<div class="cm-line">{{ "# Setting" }}</div><div class="cm-line">abc</div>';
      fr.contentWindow.focus();
      cm.focus();
      var g = fd.createRange();
      if (n === 2 || n === 12 || n === 19) { g.setStart(cm.firstChild, 12); g.collapse(true); }
      else { g.selectNodeContents(cm); g.collapse(false); }
      var s = fr.contentWindow.getSelection();
      s.removeAllRanges();
      s.addRange(g);
      var last = cm.lastChild;
      if (n === 2 || n === 12 || n === 19) {
        var q = g.getBoundingClientRect();
        cur.caretInFrame = { l: q.left, t: q.top, b: q.bottom };
      } else if (n === 5) {
        var t = fd.createRange();
        t.setStart(last.firstChild, 2);
        t.setEnd(last.firstChild, 3);
        var tr = t.getBoundingClientRect();
        cur.caretInFrame = { l: tr.right, t: tr.top, b: tr.bottom };
      } else {
        var lr = last.getBoundingClientRect();
        cur.caretInFrame = { l: lr.left, t: lr.top, b: lr.bottom };
      }
      cur.frame = rect(fr);
      cur.frameInner = fr.contentWindow.innerWidth;
      return ime({ text: 'とうきょう', caret: 5, selectedIndex: n === 2 ? 2 : 0 });
    }
    if (n === 6) {
      // A reading converted into three clauses, the second the target: the box anchors at that clause's start, as
      // the Windows IME's window does.
      cur.start = field('こんにちは、');
      cur.anchorIndex = cur.start + 3;
      return ime({ text: '今日は良い天気', caret: 7, clauses: [0, 3, 5, 7], target: 1, candidates: ['良い', 'いい', '善い'] });
    }
    if (n === 7) {
      // A zoom on <html>, not <body>, and the field at the window's right edge: the box stays in the window.
      field('こんにちは、元気');
      f0.style.left = 'auto';
      f0.style.right = '20px';
      f0.style.width = '120px';
      return ime({ text: '東京', caret: 2 });
    }
    cur.start = field('こんにちは、元気');
    cur.anchorIndex = cur.start;
    if (n === 8) f0.setAttribute('data-ml-ime', 'off');
    // 11 a 22px field, 13 the same under a zoom on <body>, 14 --ml-ime-font set on <html>, 15 a zoom on the field.
    if (n === 11 || n === 13 || n === 14) f0.style.font = '22px sans-serif';
    if (n === 15) f0.style.zoom = '1.5';   // a zoom on the field itself, none on <body>
    var hs = document.documentElement.style;
    if (n === 16 || n === 20) {   // the page themes its tooltips only (SeverActions' style.css)
      hs.setProperty('--ml-tooltip-bg', '#2a2016');
      hs.setProperty('--ml-tooltip-fg', '#e8dcc0');
      hs.setProperty('--ml-tooltip-border', '#8a6d3b');
    }
    if (n === 17) {   // a dark field on a translucent dark panel over the game (a chat overlay)
      hs.background = 'transparent';
      document.body.style.background = 'rgba(15,18,24,0.7)';
      f0.style.background = 'transparent';
      f0.style.color = '#e6e6e6';
      f0.style.border = '0';
    }
    if (n === 18) {   // a light page (SeverActions' vellum theme)
      hs.background = '#f3ecdc';
      document.body.style.background = '#f3ecdc';
      f0.style.background = '#fffaf0';
      f0.style.color = '#2b2118';
      f0.style.border = '1px solid #b8a888';
    }
    if (n === 20) {   // --ml-ime-* over everything
      hs.setProperty('--ml-ime-bg', '#000080');
      hs.setProperty('--ml-ime-fg', '#ffff00');
      hs.setProperty('--ml-ime-border', '#ff0000');
      hs.setProperty('--ml-ime-selected-bg', '#00ff00');
      hs.setProperty('--ml-ime-selected-fg', '#000000');
    }
    if (n === 21) {   // grey on grey: too close to read, so the defaults
      f0.style.background = '#777777';
      f0.style.color = '#8a8a8a';
    }
    if (n >= 22 && n <= 27) {   // the field in a chat bar, the page see-through over the game
      hs.background = 'transparent';
      document.body.style.background = 'transparent';
      chat.style.display = 'block';
      line.appendChild(f0);   // moving it drops its focus
      f0.focus();
      f0.setSelectionRange(f0.value.length, f0.value.length);
      f0.style.background = 'transparent';
      f0.style.color = 'rgba(218,210,192,0.95)';   // --chat-text
      f0.style.border = '0';
      // 22, 25 and 27: the brown on the backdrop layer, as a chat overlay draws it; 26: on the container (an ancestor); 23
      // and 24: a radial gradient, a url() over a colour, on the container.
      if (n === 22 || n === 25 || n === 27) backdrop.style.display = 'block';
      if (n === 27) vignette.style.display = 'block';
      chat.style.background = n === 23 ? 'radial-gradient(circle, rgba(40,20,60,0.9), rgba(40,20,60,0.5))'
                            : n === 24 ? 'url(nothing.png) rgba(20,40,20,0.8)'
                            // --chat-backdrop-bottom / --chat-backdrop-top
                            : n === 26 ? 'linear-gradient(to top, rgba(10,8,5,0.92), rgba(10,8,5,0.4))' : 'transparent';
      if (n === 25) {
        hs.setProperty('--ml-ime-bg', '#000080');
        hs.setProperty('--ml-ime-fg', '#ffff00');
        hs.setProperty('--ml-ime-border', '#ff0000');
      }
      // The ground under the field, worked out apart from the page core: the gradient's alpha at the field's centre,
      // over black (the game behind the overlay).
      var cr = chat.getBoundingClientRect(), fr2 = f0.getBoundingClientRect();
      cur.chatT = (cr.bottom - (fr2.top + fr2.bottom) / 2) / cr.height;
      cur.chatAlpha = 0.92 + (0.4 - 0.92) * cur.chatT;
    }
    if (n === 14) document.documentElement.style.setProperty('--ml-ime-font', '30px/1.2 serif');
    var c = n === 3 ? ['とても長い予測変換の候補がここに表示されますとても長い予測変換の候補がここに表示されます', '東京'] : nine;
    return ime({ text: '東京', caret: 2, candidates: c, end: n === 10 });
  };
  window.__imeCheck = function (n) {
    var box = document.getElementById('__ml_ime');
    var out = cur;
    out.active = (document.activeElement && document.activeElement.id) || '';
    out.box = box && box.style.display === 'block' ? rect(box) : null;
    out.events = events.length;
    var b = out.box, fail = [];
    var d = events[0] || {};
    if (!events.length) fail.push('no magelight:ime event');
    else if (n !== 10) {
      ['state', 'text', 'composition', 'caret', 'candidates', 'selectedIndex', 'clauses', 'target', 'anchor'].forEach(function (k) {
        if (!(k in d)) fail.push('the event detail has no ' + k);
      });
      if (!d.anchor || typeof d.anchor.x !== 'number' || typeof d.anchor.h !== 'number') fail.push('no anchor in the detail');
    }
    if (n === 8 || n === 9 || n === 10) {
      if (b) fail.push(n === 8 ? 'a box under data-ml-ime=off' : n === 9 ? 'a box after a cancelled event' : 'a box after an end');
      if (n === 10 && (events.length < 2 || events[events.length - 1].state !== 'end')) fail.push('no end event');
    } else if (!b) fail.push('no box');
    else if (n === 7) {
      out.corner = corner();
      out.field = rect(f0);
      if (b.l < 0 || b.t < 0 || b.r > out.corner.r + 1 || b.b > out.corner.b + 1)
        fail.push('the box leaves the window (' + b.r.toFixed(0) + ',' + b.b.toFixed(0) + ' vs ' + out.corner.r.toFixed(0) + ',' + out.corner.b.toFixed(0) + ')');
      if (out.field.t - b.b < 0 || out.field.t - b.b > 6) fail.push('not just above the field');
    } else if (out.caretInFrame) {
      // The reading's first character, measured in the frame once it is laid out (a CJK font can make the line
      // taller than the caret before it was), carried into the top page's units.
      var ch = charRect(fd, 'とうきょう');
      var s0 = fr.clientWidth / fr.contentWindow.innerWidth, ox = out.frame.l + fr.clientLeft, oy = out.frame.t + fr.clientTop;
      if (!ch) fail.push('the reading is not in the editor');
      else {
        var charBottom = oy + s0 * ch.bottom, charLeft = ox + s0 * ch.left, caretLeft = ox + s0 * out.caretInFrame.l;
        out.char = { l: charLeft, b: charBottom };
        if (Math.abs(charLeft - caretLeft) > 2) fail.push('the reading does not start at the caret');
        if (!(b.t >= charBottom && b.t - charBottom <= 6)) fail.push('not just below the reading (' + b.t.toFixed(1) + ' vs ' + charBottom.toFixed(1) + ')');
        if (Math.abs(b.l - charLeft) > 4) fail.push('not at the reading x (' + b.l.toFixed(1) + ' vs ' + charLeft.toFixed(1) + ')');
      }
    } else {
      out.field = rect(f0);
      if (n === 15) {
        // The field's rects are in units of its own zoom: carried into the box's (<body> is not zoomed here).
        ['l', 't', 'r', 'b'].forEach(function (k) { out.field[k] *= 1.5; });
      }
      var gap = out.field.t - b.b;
      if (!(gap >= 0 && gap <= 4)) fail.push('not just above the field (gap ' + gap.toFixed(1) + ')');
      out.anchorX = n === 15 ? out.field.l + 1.5 * (inputX(f0, out.anchorIndex) - rect(f0).l) : inputX(f0, out.anchorIndex);
      if (Math.abs(b.l - out.anchorX) > 4) fail.push('not at the composition x (' + b.l.toFixed(1) + ' vs ' + out.anchorX.toFixed(1) + ')');
    }
    if (b) {
      // The box's text is the field's size: a candidate's first character, carried into viewport px, against the
      // composition's (the anchor). --ml-ime-font overrides it whole.
      var cs = getComputedStyle(box), fpx = parseFloat(cs.fontSize), vv = corner().b / innerHeight;
      var row0 = box.firstChild && box.firstChild.firstChild, gh = 0;
      if (row0) {
        var at = row0.data.indexOf('. ') + 2, gr = document.createRange();
        gr.setStart(row0, at);
        gr.setEnd(row0, at + 1);
        gh = gr.getBoundingClientRect().height / vv;
      }
      out.boxFont = cs.fontSize + ' ' + cs.lineHeight + ' ' + cs.fontFamily;
      out.glyph = { box: gh, field: d.anchor ? d.anchor.h : 0 };
      if (n === 14) {
        if (cs.fontSize !== '30px' || cs.fontFamily.indexOf('serif') < 0) fail.push('--ml-ime-font does not override (' + out.boxFont + ')');
      } else if (!(Math.abs(gh - out.glyph.field) <= 2)) {
        fail.push('the box text is not the field\'s size (' + gh.toFixed(1) + ' vs ' + out.glyph.field.toFixed(1) + ' viewport px)');
      }
      if (n === 3 && b.w > Math.min(34 * fpx, corner().r * 0.9) + 1) fail.push('wider than 34em (' + b.w.toFixed(0) + ')');
    }
    if (b) {
      // The colours: the box's, its border's and the selected row's, as computed.
      var bs = getComputedStyle(box), sel = box.children[d.selectedIndex >= 0 ? d.selectedIndex : 0], ss = sel ? getComputedStyle(sel) : null;
      out.colors = { bg: bs.backgroundColor, fg: bs.color, border: bs.borderTopColor, selBg: ss && ss.backgroundColor, selFg: ss && ss.color };
      var want = {
        16: { bg: 'rgb(42, 32, 22)', fg: 'rgb(232, 220, 192)', border: 'rgb(138, 109, 59)' },
        17: { bg: 'rgb(11, 13, 17)', fg: 'rgb(230, 230, 230)' },
        18: { bg: 'rgb(255, 250, 240)', fg: 'rgb(43, 33, 24)', border: 'rgb(184, 168, 136)' },
        19: { bg: 'rgb(16, 32, 16)', fg: 'rgb(208, 240, 208)' },
        20: { bg: 'rgb(0, 0, 128)', fg: 'rgb(255, 255, 0)', border: 'rgb(255, 0, 0)', selBg: 'rgb(0, 255, 0)', selFg: 'rgb(0, 0, 0)' },
        21: { bg: 'rgb(30, 30, 32)', fg: 'rgb(240, 240, 240)', border: 'rgb(92, 92, 96)', selBg: 'rgb(92, 92, 96)', selFg: 'rgb(255, 255, 255)' },
        22: { bg: 'rgb(' + [10, 8, 5].map(function (x) { return Math.round(x * cur.chatAlpha); }).join(', ') + ')', fg: 'rgb(218, 210, 192)' },
        26: { bg: 'rgb(' + [10, 8, 5].map(function (x) { return Math.round(x * cur.chatAlpha); }).join(', ') + ')', fg: 'rgb(218, 210, 192)' },
        // The vignette in front (10% red), then the backdrop, over black.
        27: { bg: 'rgb(' + [25.5 + 0.9 * cur.chatAlpha * 10, 0.9 * cur.chatAlpha * 8, 0.9 * cur.chatAlpha * 5].map(Math.round).join(', ') + ')',
              fg: 'rgb(218, 210, 192)' },
        23: { bg: 'rgb(28, 14, 42)', fg: 'rgb(218, 210, 192)' },
        24: { bg: 'rgb(16, 32, 16)', fg: 'rgb(218, 210, 192)' },
        25: { bg: 'rgb(0, 0, 128)', fg: 'rgb(255, 255, 0)', border: 'rgb(255, 0, 0)' }
      }[n];
      if (want) for (var wk in want) if (out.colors[wk] !== want[wk]) fail.push(wk + ' ' + out.colors[wk] + ' (want ' + want[wk] + ')');
      // Whatever the colours, the text and the selected row read (3:1).
      if (n !== 20 && n !== 25) {
        var ck = function (a, c) {
          function p(s) { var m = String(s).match(/[\d.]+/g); return m ? [+m[0], +m[1], +m[2]] : [0, 0, 0]; }
          function L(c) { return c.reduce(function (s, x, i) { x /= 255; x = x <= 0.03928 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); return s + x * [0.2126, 0.7152, 0.0722][i]; }, 0); }
          var x = L(p(a)), y = L(p(c));
          return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
        };
        out.contrast = { text: ck(out.colors.fg, out.colors.bg), selected: ss ? ck(out.colors.selFg, out.colors.selBg) : 0 };
        if (out.contrast.text < 3) fail.push('the text is under 3:1 on the box');
        if (ss && out.contrast.selected < 3) fail.push('the selected row is under 3:1');
      }
    }
    if (box && box.parentNode !== document.body) fail.push('not in <body>');
    out.ok = fail.length === 0;
    out.fail = fail;
    return JSON.stringify(out);
  };
})();
