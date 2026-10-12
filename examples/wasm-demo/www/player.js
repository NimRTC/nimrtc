/* Minimal asciicast v2 player. No external deps. MIT-style. */
(function () {
  'use strict';

  function el(tag, attrs, children) {
    var e = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      if (k === 'class') e.className = attrs[k];
      else if (k === 'style') e.setAttribute('style', attrs[k]);
      else e.setAttribute(k, attrs[k]);
    }
    if (children) for (var i = 0; i < children.length; i++) {
      var c = children[i];
      if (c == null) continue;
      e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return e;
  }

  function parseCast(text) {
    var lines = text.split(/\r?\n/).filter(function (l) { return l.length > 0; });
    var headerEnd = -1;
    for (var i = 0; i < lines.length; i++) {
      if (lines[i][0] === '[') { headerEnd = i; break; }
    }
    if (headerEnd < 0) throw new Error('invalid asciicast: no events array');
    var header = JSON.parse(lines.slice(0, headerEnd).join('\n'));
    var events = [];
    for (var j = headerEnd; j < lines.length; j++) {
      var ev = JSON.parse(lines[j]);
      if (!Array.isArray(ev) || ev.length < 3) continue;
      events.push({ t: ev[0], type: ev[1], data: ev[2] });
    }
    events.sort(function (a, b) { return a.t - b.t; });
    return { header: header, events: events };
  }

  function castToTerminal(cast, container, opts) {
    opts = opts || {};
    var cols = cast.header.width  || 100;
    var rows = cast.header.height || 30;
    var fontPx = opts.fontPx || 14;
    var charW = Math.round(fontPx * 0.60);
    var charH = Math.round(fontPx * 1.25);

    // Build screen as a fixed grid: cell[y][x] = {ch, fg, bg}
    var screen = [];
    var curX = 0, curY = 0;
    for (var y = 0; y < rows; y++) {
      var row = [];
      for (var x = 0; x < cols; x++) row.push(' ');
      screen.push(row);
    }
    function advance() {
      var s = '';
      for (var y = 0; y < rows; y++) {
        for (var x = 0; x < cols; x++) s += screen[y][x];
        s += '\n';
      }
      return s;
    }
    function writeText(text) {
      for (var i = 0; i < text.length; i++) {
        var ch = text[i];
        if (ch === '\r') { curX = 0; continue; }
        if (ch === '\n') { curY++; curX = 0; if (curY >= rows) curY = 0; continue; }
        if (ch === '\x1b') {
          // skip the entire CSI/OSC escape sequence (length not strictly correct for OSC,
          // but good enough for this synthesized recording which has no real escapes)
          i++;
          while (i < text.length && text[i] !== 'm' && text[i] !== 'K' && text[i] !== 'H' && text[i] !== 'J') i++;
          continue;
        }
        screen[curY][curX] = ch;
        curX++;
        if (curX >= cols) { curX = 0; curY++; if (curY >= rows) curY = 0; }
      }
    }

    // DOM: terminal frame
    var frame = el('div', { class: 'term-frame' });
    var titleBar = el('div', { class: 'term-titlebar' }, [
      el('span', { class: 'term-dot term-dot-r' }),
      el('span', { class: 'term-dot term-dot-y' }),
      el('span', { class: 'term-dot term-dot-g' }),
      el('span', { class: 'term-title' }, 'loopback-p2p — zsh — 132x30')
    ]);
    var pre = el('pre', { class: 'term-screen' });
    pre.style.fontSize = fontPx + 'px';
    pre.style.lineHeight = charH + 'px';
    frame.appendChild(titleBar);
    frame.appendChild(pre);
    container.appendChild(frame);

    // Pre-compute cumulative text at each event so the renderer just
    // re-renders the pre tag at each event. Cheap (n<30).
    var snapshots = [];
    var snap = '';
    for (var e = 0; e < cast.events.length; e++) {
      var ev = cast.events[e];
      if (ev.type === 'o') writeText(ev.data);
      // ignore 'i' (typed but not yet echoed) — our recording has none
      snap = advance();
      snapshots.push({ t: ev.t, text: snap });
    }
    // Final state (after last event)
    snapshots.push({ t: cast.events[cast.events.length - 1].t + 0.001, text: snap });

    // Playback state
    var speed = 1.0;
    var idx = 0;
    var timer = null;
    var startedAt = 0;
    var paused = true;
    var baseT = 0; // virtual t when we resumed

    function render(i) {
      if (i < 0) i = 0;
      if (i >= snapshots.length) i = snapshots.length - 1;
      idx = i;
      // Strip the trailing blank-line row that the synthetic cast leaves
      // behind on the last frame; otherwise the terminal looks like it
      // has a stray empty line below the prompt.
      var txt = snapshots[i].text.replace(/\n+$/g, '\n');
      pre.textContent = txt;
    }

    function tick() {
      if (paused) return;
      var elapsed = (performance.now() - startedAt) / 1000 * speed;
      var vt = baseT + elapsed;
      while (idx < snapshots.length - 1 && snapshots[idx + 1].t <= vt) idx++;
      if (idx >= snapshots.length - 1) {
        render(snapshots.length - 1);
        pause();
        return;
      }
      render(idx);
      timer = requestAnimationFrame(tick);
    }

    function play() {
      if (!paused) return;
      paused = false;
      startedAt = performance.now();
      if (idx >= snapshots.length - 1) { idx = 0; baseT = 0; render(0); }
      else baseT = snapshots[idx].t;
      tick();
    }
    function pause() { paused = true; if (timer) cancelAnimationFrame(timer); timer = null; }
    function restart() { pause(); idx = 0; baseT = 0; render(0); play(); }
    function setSpeed(s) { speed = s; if (!paused) { pause(); play(); } }

    // Initial render (first snapshot only — empty screen)
    render(0);
    // Auto-play once on load
    setTimeout(play, 400);

    // Controls
    var ctrl = el('div', { class: 'term-ctrl' });
    var btnRestart = el('button', { type: 'button' }, '↻ restart');
    var btnPlay = el('button', { type: 'button' }, '⏵ play');
    function syncPlayLabel() { btnPlay.textContent = paused ? '⏵ play' : '⏸ pause'; }
    btnPlay.addEventListener('click', function () { if (paused) play(); else pause(); syncPlayLabel(); });
    btnRestart.addEventListener('click', function () { restart(); syncPlayLabel(); });
    var speedSel = el('select');
    ['0.5', '1', '2'].forEach(function (s) {
      var o = el('option', { value: s }, s + '×');
      if (s === '1') o.selected = true;
      speedSel.appendChild(o);
    });
    speedSel.addEventListener('change', function () { setSpeed(parseFloat(speedSel.value)); });
    ctrl.appendChild(btnRestart);
    ctrl.appendChild(btnPlay);
    ctrl.appendChild(el('span', { class: 'term-ctrl-label' }, 'speed'));
    ctrl.appendChild(speedSel);
    container.appendChild(ctrl);
  }

  function init() {
    var nodes = document.querySelectorAll('[data-asciicast]');
    for (var i = 0; i < nodes.length; i++) {
      (function (node) {
        var url = node.getAttribute('data-asciicast');
        var fontPx = parseInt(node.getAttribute('data-font') || '14', 10);
        fetch(url).then(function (r) { return r.text(); }).then(function (txt) {
          var cast = parseCast(txt);
          castToTerminal(cast, node, { fontPx: fontPx });
        }).catch(function (err) {
          node.textContent = 'failed to load asciicast: ' + url + ' (' + err + ')';
        });
      })(nodes[i]);
    }
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
