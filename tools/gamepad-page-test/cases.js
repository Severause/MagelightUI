// Runs before the page core's gamepad helper (src/GamepadScript.h), with a stub window.magelight that records what the
// helper sends and hands it packets on '__gamepad' as the host does (a JSON string). Each case is a list of steps;
// gamepad_page_test.exe runs one step per call to __gpStep, pumping frames between them, until a step returns
// { ok, detail }.
(function () {
  var subs = {}, sent = [];
  window.magelight = {
    on: function (ch, fn) { (subs[ch] = subs[ch] || []).push(fn); return function () {}; },
    send: function (ch, p) { sent.push({ ch: ch, p: p }); }
  };
  function deliver(obj) { (subs.__gamepad || []).forEach(function (fn) { fn(JSON.stringify(obj)); }); }

  var page = null, gen = 1, seq = 0;
  var controlOf = { up: 'dpadUp', down: 'dpadDown', left: 'dpadLeft', right: 'dpadRight', accept: 'south',
                    cancel: 'east', previousTab: 'leftShoulder', nextTab: 'rightShoulder' };
  function packet(over) {
    var p = { version: 1, page: page, generation: gen, sequence: ++seq, active: true, reset: false, enabled: true,
              connected: true, style: 'navigation', mode: 'navigation', device: 'gamepad', glyphFamily: 'xbox',
              bindings: { up: 'dpadUp', down: 'dpadDown', left: 'dpadLeft', right: 'dpadRight', accept: 'south',
                          cancel: 'east', previousTab: 'leftShoulder', nextTab: 'rightShoulder', secondary: 'west',
                          tertiary: 'north', toggleCursor: 'rightThumb' },
              dt: 0.016, events: [] };
    for (var k in over) p[k] = over[k];
    return p;
  }
  function ev(action, phase) {
    return { control: controlOf[action] || 'none', action: action, phase: phase || 'press', value: 1, x: 0, y: 0 };
  }
  function press(action, over) { var o = over || {}; o.events = [ev(action)]; deliver(packet(o)); }
  function gp() { return window.magelight.gamepad; }
  function stage(html, zoom) {
    var s = document.getElementById('stage');
    s.style.zoom = zoom || '';
    s.innerHTML = html;
    return s;
  }
  function focusedId() {
    var e = document.querySelector('[data-ml-focused]');
    return e ? (e.getAttribute('data-ml-id') || e.id) : '';
  }
  function result(ok, detail) { return { ok: !!ok, detail: String(detail) }; }
  // A 3x3 grid of buttons b0..b8, 120 px apart.
  function grid() {
    var h = '';
    for (var i = 0; i < 9; ++i)
      h += '<button id="b' + i + '" style="left:' + (100 + (i % 3) * 120) + 'px;top:' + (100 + Math.floor(i / 3) * 120) + 'px">' + i + '</button>';
    return h;
  }
  var dispose = null;
  function attach(opts) { dispose = gp().attachNavigation(opts); return dispose; }
  function done() { if (dispose) dispose(); dispose = null; }

  var cases = [
    // Before any packet (no pad in use) the helper marks its selection but takes no DOM focus, and a DOM change in
    // its scope does not pull focus out of a text field the player is typing in.
    { name: 'no_focus_taken_without_pad', steps: [
      function () {
        stage('<input type="text" id="q" style="left:600px;top:100px">' +
              '<div id="cards"><button id="c0" style="left:100px;top:100px">0</button>' +
              '<button id="c1" style="left:100px;top:160px">1</button></div>');
        document.getElementById('q').focus();
        attach({ root: document.getElementById('cards'), initialFocus: 'c1' });
        document.getElementById('c1').remove();   // the selected card filtered out while typing
      },
      function () {
        var active = document.activeElement && document.activeElement.id, marked = focusedId();
        done();
        return result(active === 'q' && marked === 'c0', 'active=' + active + ' marked=' + marked);
      }
    ] },

    { name: 'ready_sent_once', steps: [function () {
      var ready = sent.filter(function (m) { return m.ch === '__gamepad'; })
                      .map(function (m) { return JSON.parse(m.p); })
                      .filter(function (o) { return o.op === 'ready'; });
      page = ready.length ? ready[0].page : null;
      return result(ready.length === 1 && /^[A-Za-z0-9_-]{1,80}$/.test(page || '') && gp() && gp().version === 1,
                    'ready=' + ready.length + ' page=' + page);
    }] },

    { name: 'grid_navigates_by_geometry', steps: [function () {
      var s = stage(grid());
      attach({ root: s, initialFocus: 'b4' });
      var start = focusedId();
      press('right');
      var right = focusedId();
      press('up');
      var up = focusedId();
      document.getElementById('b2').setAttribute('data-ml-left', 'b6');
      press('left');
      var over = focusedId();
      done();
      return result(start === 'b4' && right === 'b5' && up === 'b2' && over === 'b6',
                    [start, right, up, over].join(' '));
    }] },

    { name: 'zoomed_grid_navigates_by_sight', steps: [function () {
      var s = stage(grid(), '1.5');
      attach({ root: s, initialFocus: 'b0' });
      press('right'); var a = focusedId();
      press('down'); var b = focusedId();
      press('left'); var c = focusedId();
      done();
      return result(a === 'b1' && b === 'b4' && c === 'b3', [a, b, c].join(' '));
    }] },

    // A zoomed panel beside unzoomed controls: the zoomed button sits at 300 px in its own units but draws at 600 px,
    // so the unzoomed button at 450 px is the nearer one to the right. Rects that ignored zoom would pick the other.
    { name: 'mixed_zoom_navigates_by_sight', steps: [function () {
      stage('<button id="z0" style="left:100px;top:100px">a</button>' +
            '<button id="z1" style="left:450px;top:100px">b</button>' +
            '<div style="position:absolute;left:0;top:0;zoom:2"><button id="z2" style="left:300px;top:50px">c</button></div>');
      attach({ root: document.getElementById('stage'), initialFocus: 'z0' });
      press('right'); var a = focusedId();
      press('right'); var b = focusedId();
      done();
      return result(a === 'z1' && b === 'z2', a + ' ' + b);
    }] },

    { name: 'modal_scope_restores_selection', steps: [function () {
      stage('<div id="list">' +
            '<button id="l0" style="left:100px;top:100px">0</button><button id="l1" style="left:100px;top:160px">1</button>' +
            '<button id="l2" style="left:100px;top:220px">2</button></div>' +
            '<div id="modal"><button id="m0" style="left:600px;top:100px">m0</button>' +
            '<button id="m1" style="left:600px;top:160px">m1</button></div>');
      attach({ root: document.getElementById('list'), initialFocus: 'l2' });
      var parent = dispose;
      var closeModal = gp().attachNavigation({ root: document.getElementById('modal') });
      var inModal = focusedId();
      press('down');
      var moved = focusedId();
      closeModal();
      var back = focusedId();
      dispose = parent;
      done();
      return result(inModal === 'm0' && moved === 'm1' && back === 'l2', [inModal, moved, back].join(' '));
    }] },

    { name: 'recovers_after_dom_change', steps: [
      function () {
        var html = '';
        for (var i = 0; i < 4; ++i) html += '<button data-ml-id="a' + i + '" style="left:100px;top:' + (100 + i * 60) + 'px">' + i + '</button>';
        stage('<div id="ls">' + html + '</div>');
        attach({ root: document.getElementById('ls') });
        press('down');
        window.__gpHtml = html;
        document.getElementById('ls').innerHTML = html;   // re-rendered: the same ids, new elements
      },
      function () {
        var kept = focusedId();
        var a1 = document.querySelector('[data-ml-id="a1"]');
        a1.parentNode.removeChild(a1);
        window.__gpKept = kept;
      },
      function () {
        var after = focusedId();
        var el = document.querySelector('[data-ml-focused]');
        done();
        return result(window.__gpKept === 'a1' && after !== '' && after !== 'a1' && el && el.isConnected,
                      window.__gpKept + ' ' + after);
      }
    ] },

    { name: 'range_and_select_edit_mode', steps: [function () {
      stage('<input type="range" id="r" min="0" max="10" step="2" value="4" style="left:100px;top:100px">' +
            '<select id="s" style="left:300px;top:100px"><option>a</option><option>b</option><option>c</option></select>');
      var r = document.getElementById('r'), inputs = 0, changes = 0, backs = 0;
      r.addEventListener('input', function () { ++inputs; });
      r.addEventListener('change', function () { ++changes; });
      attach({ root: document.getElementById('stage'), initialFocus: 'r', onBack: function () { ++backs; } });
      press('accept');
      press('right');
      var value = r.value;
      press('cancel');
      var backsAfterLeave = backs;
      press('right');
      var next = focusedId();
      var valueAfter = r.value;
      press('accept');
      press('down');
      var sel = document.getElementById('s').selectedIndex;
      press('cancel');
      press('cancel');
      done();
      return result(value === '6' && inputs === 1 && changes === 1 && backsAfterLeave === 0 && next === 's' &&
                    valueAfter === '6' && sel === 1 && backs === 1,
                    'value=' + value + ' inputs=' + inputs + ' changes=' + changes + ' backs=' + backsAfterLeave + '/' +
                    backs + ' next=' + next + ' sel=' + sel);
    }] },

    // A div marked data-ml-focusable, with no tabindex, is a control the pad can reach and focus.
    { name: 'focusable_marker_without_tabindex', steps: [function () {
      stage('<button id="f0" style="left:100px;top:100px">a</button>' +
            '<div id="f1" data-ml-focusable style="position:absolute;left:300px;top:100px;width:80px;height:40px">b</div>');
      attach({ root: document.getElementById('stage'), initialFocus: 'f0' });
      press('right');
      var to = focusedId(), active = document.activeElement && document.activeElement.id;
      done();
      return result(to === 'f1' && active === 'f1', to + ' active=' + active);
    }] },

    // A select edited with the pad fires input before change, as a browser does.
    { name: 'select_edit_fires_input', steps: [function () {
      stage('<select id="se" style="left:100px;top:100px"><option>a</option><option>b</option></select>');
      var s = document.getElementById('se'), events = [];
      s.addEventListener('input', function () { events.push('input'); });
      s.addEventListener('change', function () { events.push('change'); });
      attach({ root: document.getElementById('stage'), initialFocus: 'se' });
      press('accept');
      press('down');
      press('cancel');
      done();
      return result(events.join(',') === 'input,change' && s.selectedIndex === 1, events.join(',') + ' sel=' + s.selectedIndex);
    }] },

    { name: 'text_field_ignores_directions', steps: [function () {
      stage('<input type="text" id="t" style="left:100px;top:100px"><button id="tb" style="left:100px;top:200px">b</button>');
      attach({ root: document.getElementById('stage'), initialFocus: 't' });
      press('accept');
      press('down');
      var editing = focusedId();
      press('cancel');
      press('down');
      var after = focusedId();
      done();
      return result(editing === 't' && after === 'tb', editing + ' ' + after);
    }] },

    { name: 'tabs_cycle_on_shoulders', steps: [function () {
      var h = '';
      for (var i = 0; i < 3; ++i)
        h += '<button role="tab" id="tab' + i + '" aria-selected="' + (i === 0) + '" style="left:' + (100 + i * 120) + 'px;top:50px">t' + i + '</button>';
      stage(h);
      var tabs = document.querySelectorAll('[role=tab]');
      tabs.forEach(function (t) {
        t.addEventListener('click', function () {
          tabs.forEach(function (o) { o.setAttribute('aria-selected', String(o === t)); });
        });
      });
      attach({ root: document.getElementById('stage'), initialFocus: 'tab0' });
      press('nextTab');
      var a = document.querySelector('[aria-selected=true]').id;
      press('nextTab');
      var b = document.querySelector('[aria-selected=true]').id;
      press('previousTab');
      var c = document.querySelector('[aria-selected=true]').id;
      done();
      return result(a === 'tab1' && b === 'tab2' && c === 'tab1', [a, b, c].join(' '));
    }] },

    { name: 'keys_style_state_only', steps: [function () {
      var s = stage(grid());
      attach({ root: s, initialFocus: 'b4' });
      deliver(packet({ style: 'keys', device: 'gamepad', events: [ev('right')] }));
      var still = focusedId();
      var device = gp().getState().device, attr = document.documentElement.getAttribute('data-ml-input');
      deliver(packet({ style: 'navigation', device: 'keyboardMouse', events: [] }));
      var attr2 = document.documentElement.getAttribute('data-ml-input');
      done();
      return result(still === 'b4' && device === 'gamepad' && attr === 'gamepad' && attr2 === 'keyboardMouse',
                    [still, device, attr, attr2].join(' '));
    }] },

    { name: 'prompts_follow_glyphs', steps: [function () {
      var labels = ['xbox', 'playstation', 'generic'].map(function (g) {
        deliver(packet({ glyphFamily: g }));
        return gp().getPrompt('accept').label;
      });
      deliver(packet({ glyphFamily: 'xbox' }));
      return result(labels[0] === 'A' && labels[1] === 'Cross' && labels[2] === 'South', labels.join(' '));
    }] },

    { name: 'stale_and_oversized_packets_dropped', steps: [function () {
      var s = stage(grid());
      attach({ root: s, initialFocus: 'b4' });
      gen = 2;
      deliver(packet({ events: [] }));
      var seen = [];
      deliver(packet({ page: 'someone-else', events: [ev('right')] })); seen.push(focusedId());
      deliver(packet({ generation: 1, events: [ev('right')] })); seen.push(focusedId());
      var p = packet({ events: [ev('right')] });
      deliver(p);
      var b5 = focusedId();
      deliver(p); seen.push(focusedId());   // the same sequence again
      var many = [];
      for (var i = 0; i < 129; ++i) many.push(ev('left'));
      deliver(packet({ events: many })); seen.push(focusedId());
      var bad = ev('left'); bad.x = NaN;   // JSON carries it as null
      deliver(packet({ events: [bad] })); seen.push(focusedId());
      done();
      return result(seen[0] === 'b4' && seen[1] === 'b4' && b5 === 'b5' && seen[2] === 'b5' && seen[3] === 'b5' &&
                    seen[4] === 'b5', seen.join(' ') + ' b5=' + b5);
    }] },

    { name: 'cancel_broadcast_on_reset', steps: [function () {
      var got = [];
      var stop = gp().onAction(function (e) { got.push(e.phase); return false; });
      deliver(packet({ reset: true, events: [] }));
      stop();
      return result(got.indexOf('cancel') >= 0, got.join(' '));
    }] }
  ];

  window.__gpCount = function () { return cases.length; };
  window.__gpName = function (n) { return cases[n].name; };
  window.__gpStep = function (n, s) {
    var c = cases[n];
    if (s >= c.steps.length) return JSON.stringify(result(false, 'no verdict'));
    var r = c.steps[s]();
    return r === undefined ? 'next' : JSON.stringify(r);
  };
})();
