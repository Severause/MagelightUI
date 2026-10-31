// MgLoadTest: a page whose cost per frame is chosen by a button, for comparing Magelight's cost lines with
// "pageThread" off and on. console.warn reaches Magelight.log by default, so the log ties each cost line to the load.
(function () {
  'use strict';
  var burnMs = 0;
  var layoutOn = false;
  var frames = 0;
  var lastFps = performance.now();
  var boxes = [];

  function log(msg) { console.warn('[MgLoadTest] ' + msg); }

  function choices(rowId, values, label, pick) {
    var row = document.getElementById(rowId);
    var buttons = values.map(function (v) {
      var b = document.createElement('button');
      b.textContent = label(v);
      b.addEventListener('click', function () {
        buttons.forEach(function (x) { x.classList.remove('on'); });
        b.classList.add('on');
        pick(v);
      });
      row.appendChild(b);
      return b;
    });
    buttons[0].classList.add('on');
  }

  choices('script', [0, 2, 5, 10, 20], function (v) { return v + ' ms'; }, function (v) {
    burnMs = v;
    document.getElementById('burn').textContent = v;
    log('script load ' + v + ' ms a frame');
  });
  choices('layout', [false, true], function (v) { return v ? 'on' : 'off'; }, function (v) {
    layoutOn = v;
    document.getElementById('lay').textContent = v ? 'on' : 'off';
    log('layout load ' + (v ? 'on (300 boxes)' : 'off'));
  });

  // Two clicks: the first arms it, the second hangs the page for good (a script that never returns). The game must
  // go on; with pageThread on, Magelight.log names the stall once.
  var hang = document.getElementById('hang');
  hang.addEventListener('click', function () {
    if (!hang.classList.contains('armed')) {
      hang.classList.add('armed');
      hang.textContent = 'Click again: the page freezes for good';
      return;
    }
    log('hanging the page now (a script that never returns)');
    for (;;) { /* never returns */ }
  });

  var field = document.getElementById('field');
  for (var i = 0; i < 300; i++) {
    var d = document.createElement('div');
    d.className = 'box';
    field.appendChild(d);
    boxes.push(d);
  }

  function frame(now) {
    if (burnMs > 0) {
      var until = performance.now() + burnMs;
      var x = 0;
      while (performance.now() < until) x += Math.sqrt(x + 1);
      window.__sink = x;
    }
    if (layoutOn) {
      var w = field.clientWidth, h = field.clientHeight;
      for (var j = 0; j < boxes.length; j++) {
        var t = now / 900 + j * 0.37;
        boxes[j].style.left = ((Math.sin(t) * 0.5 + 0.5) * (w - 10)) + 'px';
        boxes[j].style.top = ((Math.cos(t * 1.3) * 0.5 + 0.5) * (h - 10)) + 'px';
      }
    }
    frames++;
    if (now - lastFps >= 1000) {
      document.getElementById('fps').textContent = Math.round(frames * 1000 / (now - lastFps));
      frames = 0;
      lastFps = now;
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
  log('page up');
})();
