// Runs before the page core's accessibility reader (src/A11yScript.h), with a stub window.magelight that records what
// the reader sends on '__a11y' and hands it the host's '__a11y' messages. Each case is a list of steps; a11y_test.exe
// calls __a11yStep once per step, pumping frames between calls: a step returns 'next', 'wait' (call it again) or the
// verdict { ok, detail }.
(function () {
  var subs = {}, sent = [];
  window.magelight = {
    on: function (ch, fn) { (subs[ch] = subs[ch] || []).push(fn); return function () {}; },
    send: function (ch, p) { if (ch === '__a11y') sent.push(p); }
  };
  function host(msg) { (subs.__a11y || []).forEach(function (fn) { fn(msg); }); }
  function msgs(type) {
    var out = [];
    sent.forEach(function (s) {
      var m = JSON.parse(s);
      if (s.indexOf('{"v":1,"type":"' + m.type + '"') !== 0) throw new Error('message lacks its prefix: ' + s);
      if (!type || m.type === type) out.push(m);
    });
    return out;
  }
  function last(type) { var l = msgs(type); return l.length ? l[l.length - 1] : null; }
  function clear() { sent.length = 0; }
  function stage(html) { var s = document.getElementById('stage'); s.innerHTML = html; return s; }
  function $(id) { return document.getElementById(id); }
  function result(ok, detail) { return { ok: !!ok, detail: String(detail) }; }
  function show(m) { return JSON.stringify(m); }
  function blur() { if (document.activeElement && document.activeElement.blur) document.activeElement.blur(); }
  // A step that holds until ms have passed since it first ran (a live region's 250 ms of quiet).
  var waitStart = 0;
  function after(ms) {
    if (!waitStart) waitStart = Date.now();
    if (Date.now() - waitStart < ms) return false;
    waitStart = 0;
    return true;
  }
  function begin(html) { host('off'); blur(); stage(html || ''); clear(); host('on'); }

  var cases = [
    { name: 'idle_until_on', steps: [
      function () { host('off'); stage('<button id="b">Save</button>'); clear(); $('b').focus(); return 'next'; },
      function () { return result(sent.length === 0, 'sent ' + sent.length + ' while off'); }
    ] },
    { name: 'button_focus', steps: [
      function () { begin('<button id="b" title="Writes the file">Save <span aria-hidden="true">*</span></button>'); $('b').focus(); return 'next'; },
      function () {
        var m = last('focus');
        return result(m && m.name === 'Save' && m.role === 'button' && m.description === 'Writes the file', show(m));
      }
    ] },
    { name: 'labelled_field', steps: [
      function () {
        begin('<label for="n">Character name</label><input id="n" value="Lydia" required>');
        $('n').focus(); return 'next';
      },
      function () {
        var m = last('focus');
        return result(m && m.name === 'Character name' && m.role === 'textbox' && m.value === 'Lydia' &&
                      m.states.indexOf('required') >= 0, show(m));
      }
    ] },
    { name: 'password_value_never_sent', steps: [
      function () { begin('<input id="p" type="password" aria-label="Passphrase" value="hunter2">'); $('p').focus(); return 'next'; },
      function () {
        $('p').value = 'hunter22';
        $('p').dispatchEvent(new Event('input', { bubbles: true }));
        return 'next';
      },
      function () {
        var leak = sent.some(function (s) { return s.indexOf('hunter') >= 0; });
        var f = last('focus'), e = last('edit');
        return result(!leak && f && f.secret === true && f.length === 7 && e && e.secret === true && e.length === 8 &&
                      e.value === undefined, show(f) + ' ' + show(e));
      }
    ] },
    { name: 'text_edit', steps: [
      function () { begin('<input id="q" aria-label="Search">'); $('q').focus(); clear(); return 'next'; },
      function () { $('q').value = 'iron'; $('q').dispatchEvent(new Event('input', { bubbles: true })); return 'next'; },
      function () { var m = last('edit'); return result(m && m.name === 'Search' && m.value === 'iron', show(m)); }
    ] },
    { name: 'label_wraps_field_and_buttons', steps: [
      function () {
        begin('<label><svg width="8" height="8"></svg><input id="q" placeholder="Search items"><button type="button" aria-label="Clear">x</button>' +
              '<div role="listbox" aria-label="Recent searches"><button type="button" role="option">iron</button></div></label>' +
              '<label>Name <input id="n" value="Lydia"><button type="button">Clear</button></label>');
        $('q').focus(); clear(); return 'next';
      },
      function () { $('q').value = 'd'; $('q').dispatchEvent(new Event('input', { bubbles: true })); return 'next'; },
      function () { $('n').focus(); return 'next'; },
      function () {
        var e = last('edit'), f = last('focus');
        return result(e && e.name === 'Search items' && e.value === 'd' && f && f.name === 'Name' && f.value === 'Lydia',
                      show(e) + ' ' + show(f));
      }
    ] },
    { name: 'edit_said_once', steps: [
      function () { begin('<input id="q" aria-label="Search">'); $('q').focus(); clear(); return 'next'; },
      function () {
        var q = $('q');
        q.value = 'daedric'; q.dispatchEvent(new Event('input', { bubbles: true }));
        q.dispatchEvent(new Event('change', { bubbles: true }));
        q.value = 'daedric bow'; q.dispatchEvent(new Event('input', { bubbles: true }));
        return 'next';
      },
      function () {
        var l = msgs('edit');
        return result(l.length === 2 && l[0].value === 'daedric' && l[1].value === 'daedric bow', show(l));
      }
    ] },
    { name: 'checkbox_change', steps: [
      function () { begin('<label><input id="c" type="checkbox"> Auto save</label>'); $('c').focus(); return 'next'; },
      function () {
        var f = last('focus');
        if (!f || f.name !== 'Auto save' || f.states.indexOf('not checked') < 0) return result(false, 'focus ' + show(f));
        clear(); $('c').click(); return 'next';
      },
      function () { var m = last('state'); return result(m && m.role === 'checkbox' && m.states.indexOf('checked') >= 0, show(m)); }
    ] },
    { name: 'activedescendant', steps: [
      function () {
        begin('<div id="lb" role="listbox" tabindex="0" aria-label="Spells" aria-activedescendant="o1">' +
              '<div id="o1" role="option">Flames</div><div id="o2" role="option">Frostbite</div><div id="o3" role="option">Sparks</div></div>');
        $('lb').focus(); return 'next';
      },
      function () {
        var f = last('focus');
        if (!f || f.name !== 'Spells' || !f.active || f.active.name !== 'Flames') return result(false, 'focus ' + show(f));
        clear(); $('lb').setAttribute('aria-activedescendant', 'o2'); return 'next';
      },
      function () {
        var m = last('active');
        return result(m && m.name === 'Frostbite' && m.role === 'option' && m.position === 2 && m.setSize === 3, show(m));
      }
    ] },
    { name: 'selection_without_focus', steps: [
      function () {
        begin('<div role="tablist"><div role="tab" id="t1" aria-selected="true">Items</div>' +
              '<div role="tab" id="t2" aria-selected="false">Magic</div></div>');
        return 'next';
      },
      function () { $('t1').setAttribute('aria-selected', 'false'); $('t2').setAttribute('aria-selected', 'true'); return 'next'; },
      function () {
        var m = last('active');
        return result(m && m.name === 'Magic' && m.role === 'tab' && m.states.indexOf('selected') >= 0 && m.position === 2, show(m));
      }
    ] },
    { name: 'state_of_focused', steps: [
      function () { begin('<button id="m" aria-expanded="false">Menu</button>'); $('m').focus(); clear(); return 'next'; },
      function () { $('m').setAttribute('aria-expanded', 'true'); return 'next'; },
      function () { var m = last('state'); return result(m && m.name === 'Menu' && m.states.indexOf('expanded') >= 0, show(m)); }
    ] },
    { name: 'live_region_settles_once', steps: [
      function () { begin('<div id="log" role="log" aria-label="Chat"></div>'); return 'next'; },
      function () {
        var p = document.createElement('p'); p.textContent = 'Lydia: '; $('log').appendChild(p);
        var t = document.createTextNode('I am'); p.appendChild(t); window.__t = t; return 'next';
      },
      function () { window.__t.data += ' sworn'; return 'next'; },
      function () { window.__t.data += ' to carry your burdens.'; return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () {
        var l = msgs('announce');
        return result(l.length === 1 && l[0].text === 'Lydia: I am sworn to carry your burdens.' &&
                      l[0].politeness === 'polite' && l[0].role === 'log' && l[0].name === 'Chat', show(l));
      }
    ] },
    { name: 'alert_inserted', steps: [
      function () { begin(''); return 'next'; },
      function () {
        var a = document.createElement('div'); a.setAttribute('role', 'alert'); a.textContent = 'Not enough gold';
        $('stage').appendChild(a); return 'next';
      },
      function () { return after(400) ? 'next' : 'wait'; },
      function () {
        var m = last('announce');
        return result(m && m.text === 'Not enough gold' && m.politeness === 'assertive', show(m));
      }
    ] },
    { name: 'hidden_region_silent', steps: [
      function () { begin('<div id="s" aria-live="polite" style="display:none"></div>'); return 'next'; },
      function () { $('s').textContent = 'quiet'; return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { return result(msgs('announce').length === 0, show(msgs())); }
    ] },
    { name: 'dialog_opens', steps: [
      function () { begin(''); return 'next'; },
      function () {
        $('stage').innerHTML = '<div role="dialog" aria-modal="true" aria-labelledby="h" aria-describedby="d">' +
          '<h2 id="h">Dismiss Lydia?</h2><p id="d">She goes home to Breezehome.</p><button id="ok">Dismiss</button></div>';
        $('ok').focus(); return 'next';
      },
      function () {
        var d = last('dialog'), f = last('focus');
        return result(d && d.name === 'Dismiss Lydia?' && d.role === 'dialog' && d.description === 'She goes home to Breezehome.' &&
                      f && f.name === 'Dismiss', show(d) + ' ' + show(f));
      }
    ] },
    { name: 'opt_out', steps: [
      function () {
        begin('<div data-ml-a11y="off"><button id="x">Secret</button><div id="r" aria-live="polite"></div></div>');
        $('x').focus(); $('r').textContent = 'hush'; return 'next';
      },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { return result(sent.length === 0, show(msgs())); }
    ] },
    { name: 'page_answer', steps: [
      function () { begin('<button id="b">Close</button>'); $('b').focus(); clear(); host('page'); return 'next'; },
      function () {
        var m = last('page');
        return result(m && m.title === 'Reader test page' && m.focus && m.focus.name === 'Close', show(m));
      }
    ] },
    { name: 'page_announce', steps: [
      function () { begin(''); window.magelight.a11y.announce('Quest updated', { assertive: true }); return 'next'; },
      function () {
        var m = last('announce');
        return result(m && m.text === 'Quest updated' && m.politeness === 'assertive' && m.source === 'page', show(m));
      }
    ] },
    { name: 'iframe_focus', steps: [
      function () { begin('<iframe id="f" srcdoc="<button id=in>Inside</button>"></iframe>'); return 'next'; },
      function () {
        var d = $('f').contentDocument;
        if (!d || !d.getElementById('in')) return 'wait';
        d.getElementById('in').focus(); return 'next';
      },
      function () { var m = last('focus'); return result(m && m.name === 'Inside' && m.role === 'button', show(m)); }
    ] },
    { name: 'heading_and_select', steps: [
      function () {
        begin('<h3 id="h" tabindex="-1">Inventory</h3><select id="s" aria-label="Sort"><option>Name</option><option selected>Weight</option></select>');
        $('h').focus(); return 'next';
      },
      function () {
        var h = last('focus');
        if (!h || h.role !== 'heading' || h.level !== 3 || h.name !== 'Inventory') return result(false, show(h));
        $('s').focus(); return 'next';
      },
      function () { var m = last('focus'); return result(m && m.role === 'combobox' && m.name === 'Sort' && m.value === 'Weight', show(m)); }
    ] },
    // A stream longer than the 1.5 s cap is read in parts, each part once.
    { name: 'long_stream_read_once', steps: [
      function () {
        begin('<div id="log" role="log"></div>');
        var p = document.createElement('p'); var t = document.createTextNode('one'); p.appendChild(t);
        $('log').appendChild(p); window.__t = t; window.__n = 1; window.__words = ['one']; window.__at = Date.now();
        return 'next';
      },
      function () {
        if (window.__n >= 12) return 'next';
        if (Date.now() - window.__at < 200) return 'wait';
        window.__at = Date.now(); var w = 'w' + window.__n++; window.__words.push(w); window.__t.data += ' ' + w; return 'wait';
      },
      function () { return after(500) ? 'next' : 'wait'; },
      function () {
        var l = msgs('announce'), all = l.map(function (m) { return m.text; }).join(' ').replace(/\s+/g, ' ').trim();
        return result(l.length >= 2 && all === window.__words.join(' '), l.length + ' parts: ' + all);
      }
    ] },
    // More than 50 changes in one window: nothing is dropped.
    { name: 'many_nodes_kept', steps: [
      function () {
        begin('<div id="log" aria-live="polite"><p id="msg"></p></div>');
        var words = [];
        for (var i = 0; i < 80; i++) { var sp = document.createElement('span'); sp.textContent = 'w' + i + ' '; $('msg').appendChild(sp); words.push('w' + i); }
        window.__words = words; return 'next';
      },
      function () { return after(400) ? 'next' : 'wait'; },
      function () {
        var all = msgs('announce').map(function (m) { return m.text; }).join(' ').replace(/\s+/g, ' ').trim();
        return result(all === window.__words.join(' '), all.slice(0, 200));
      }
    ] },
    { name: 'labelledby_hidden_target', steps: [
      function () {
        begin('<button id="x" aria-labelledby="l" aria-describedby="d">X</button><span id="l" hidden>Close inventory</span>' +
              '<span id="d" style="display:none">Returns to the game</span>');
        $('x').focus(); return 'next';
      },
      function () { var m = last('focus'); return result(m && m.name === 'Close inventory' && m.description === 'Returns to the game', show(m)); }
    ] },
    { name: 'status_not_named_by_content', steps: [
      function () { begin('<div id="s" role="status"></div>'); return 'next'; },
      function () { $('s').textContent = 'Saved'; return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { var m = last('announce'); return result(m && m.text === 'Saved' && m.name === undefined, show(m)); }
    ] },
    { name: 'roving_focus_said_once', steps: [
      function () {
        begin('<div role="tablist"><button role="tab" id="a" aria-selected="true">Items</button>' +
              '<button role="tab" id="b" aria-selected="false" tabindex="-1">Magic</button></div>');
        $('a').focus(); clear(); return 'next';
      },
      function () { $('a').setAttribute('aria-selected', 'false'); $('b').setAttribute('aria-selected', 'true'); $('b').focus(); return 'next'; },
      function () {
        var l = msgs().filter(function (m) { return m.name === 'Magic'; });
        return result(l.length === 1 && l[0].type === 'focus', show(msgs()));
      }
    ] },
    { name: 'details_toggle', steps: [
      function () { begin('<details id="dt"><summary id="sm">Quest notes</summary><p>Find the Jarl.</p></details>'); $('sm').focus(); clear(); return 'next'; },
      function () { $('dt').open = true; return 'next'; },
      function () { var m = last('state'); return result(m && m.name === 'Quest notes' && m.states.indexOf('expanded') >= 0, show(m)); }
    ] },
    { name: 'no_lone_surrogate', steps: [
      function () {
        var v = new Array(999).join('a') + '\ud83d\ude00' + 'aaaaa';   // 998 a, an emoji across the 1000-unit cut
        begin('<input id="q" aria-label="Notes">'); $('q').value = v; $('q').focus(); return 'next';
      },
      function () {
        var raw = sent.filter(function (s) { return s.indexOf('"type":"focus"') > 0; }).pop() || '';
        var lone = /[\ud800-\udbff](?![\udc00-\udfff])|(^|[^\ud800-\udbff])[\udc00-\udfff]/.test(raw) || /\\ud[89ab]/i.test(raw);
        var m = raw ? JSON.parse(raw) : null;
        return result(raw && !lone && m.value.length < 1000, 'len ' + (m && m.value.length) + ' lone ' + lone);
      }
    ] },
    { name: 'wide_text_fits_cap', steps: [
      function () {
        var cjk = new Array(600).join('\u5b57');
        begin('<div id="lb" role="listbox" tabindex="0" aria-label="' + cjk + '" aria-describedby="d" aria-activedescendant="o">' +
              '<div id="o" role="option" aria-label="' + cjk + '" title="' + cjk + '">x</div></div><span id="d">' + cjk + '</span>');
        $('lb').focus(); return 'next';
      },
      function () {
        var raw = sent.filter(function (s) { return s.indexOf('"type":"focus"') > 0; }).pop() || '';
        var b = new TextEncoder().encode(raw).length, m = raw ? JSON.parse(raw) : null;
        return result(raw && b <= 8000 && m.name.length > 0, b + ' bytes');
      }
    ] },
    { name: 'same_value_again_silent', steps: [
      function () { begin('<div role="tablist"><div role="tab" id="t" aria-selected="true">Items</div></div>'); return 'next'; },
      function () { $('t').setAttribute('aria-selected', 'true'); return 'next'; },
      function () { return result(sent.length === 0, show(msgs())); }
    ] },
    // What a region held before the reader saw it is history, not news.
    { name: 'history_not_reread', steps: [
      function () {
        host('off'); blur();
        stage('<div id="log" role="log"><p>Old one</p><p id="p2">Old two</p></div>'); clear(); host('on'); return 'next';
      },
      function () { $('log').setAttribute('aria-busy', 'true'); var p = document.createElement('p'); p.textContent = 'New line'; $('log').appendChild(p); return 'next'; },
      function () { $('log').setAttribute('aria-busy', 'false'); return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () {
        var a = msgs('announce'); if (a.length !== 1 || a[0].text !== 'New line') return result(false, show(a));
        clear(); $('p2').firstChild.data += ' and more'; return 'next';
      },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { var a = msgs('announce'); return result(a.length === 1 && a[0].text === 'and more', show(a)); }
    ] },
    { name: 'region_added_with_history', steps: [
      function () { begin(''); var d = document.createElement('div'); d.id = 'chat'; d.setAttribute('role', 'log'); d.innerHTML = '<p>Earlier</p>'; $('stage').appendChild(d); return 'next'; },
      function () { var p = document.createElement('p'); p.textContent = 'Now'; $('chat').appendChild(p); return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { var a = msgs('announce'); return result(a.length === 1 && a[0].text === 'Now', show(a)); }
    ] },
    { name: 'control_in_region', steps: [
      function () { begin('<div id="r" aria-live="polite"></div>'); return 'next'; },
      function () { $('r').innerHTML = 'Sort by <select><option>Name</option><option selected>Weight</option></select>'; return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { var m = last('announce'); return result(m && m.text === 'Sort by Weight', show(m)); }
    ] },
    { name: 'off_takes_everything_down', steps: [
      function () { begin('<button id="b">One</button><div id="r" aria-live="polite"></div>'); host('off'); clear(); return 'next'; },
      function () { $('b').focus(); $('r').textContent = 'gone'; window.magelight.a11y.announce('nothing'); return 'next'; },
      function () { return after(400) ? 'next' : 'wait'; },
      function () { return result(sent.length === 0, show(msgs())); }
    ] },
    { name: 'on_again', steps: [
      function () { host('on'); blur(); stage('<button id="b">Again</button>'); clear(); $('b').focus(); return 'next'; },
      function () {
        var l = msgs('focus');
        return result(l.length === 1 && l[0].name === 'Again', show(l));
      }
    ] }
  ];

  window.__a11yCount = function () { return cases.length; };
  window.__a11yName = function (n) { return cases[n].name; };
  window.__a11yStep = function (n, s) {
    var steps = cases[n].steps;
    if (s >= steps.length) return JSON.stringify(result(false, 'ran out of steps'));
    var r = steps[s]();
    return typeof r === 'string' ? r : JSON.stringify(r);
  };
})();
