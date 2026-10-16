// Browser front end for the WebAssembly build of the order book.
// All matching happens inside lob.wasm; this file only drives it and draws.
'use strict';

(async function main() {
  const $ = (id) => document.getElementById(id);
  const fmtInt = (n) => n.toLocaleString('en-US');

  let M;
  try {
    M = await createLob(); // eslint-disable-line no-undef
  } catch (err) {
    $('loading').textContent = `The engine failed to load: ${err && err.message ? err.message : err}`;
    $('loading').classList.add('error');
    return;
  }

  const api = {
    init: M.cwrap('lob_init', 'number', ['number', 'number', 'number']),
    submit: M.cwrap('lob_submit', 'number', ['number', 'number', 'number', 'number']),
    filled: M.cwrap('lob_last_filled', 'number', []),
    resting: M.cwrap('lob_last_resting', 'number', []),
    tradeCount: M.cwrap('lob_trade_count', 'number', []),
    tradeMaker: M.cwrap('lob_trade_maker', 'number', ['number']),
    tradePrice: M.cwrap('lob_trade_price', 'number', ['number']),
    tradeQty: M.cwrap('lob_trade_qty', 'number', ['number']),
    cancel: M.cwrap('lob_cancel', 'number', ['number']),
    openQty: M.cwrap('lob_open_qty', 'number', ['number']),
    bestBid: M.cwrap('lob_best_bid', 'number', []),
    bestAsk: M.cwrap('lob_best_ask', 'number', []),
    restingCount: M.cwrap('lob_resting', 'number', []),
    depth: M.cwrap('lob_depth', 'number', ['number', 'number']),
    depthPtr: M.cwrap('lob_depth_ptr', 'number', []),
    bench: M.cwrap('lob_bench', 'number', ['number']),
    benchResult: M.cwrap('lob_bench_result', 'number', ['number']),
  };

  const MIN = 1;
  const MAX = 20000;
  const LEVELS = 12;
  api.init(MIN, MAX, 400000);

  // ── State ────────────────────────────────────────────────────────────
  const mine = new Map(); // id -> { side, price }
  const simIds = [];
  const tape = [];
  let trades = 0;
  let processed = 0;
  let lastTradePrice = 0;
  let fair = 10000;
  let dirty = true;

  function readDepth(side) {
    const n = api.depth(side, LEVELS);
    const ptr = api.depthPtr();
    const out = [];
    for (let i = 0; i < n; i++) {
      out.push([M.getValue(ptr + 8 * i, 'i32'), M.getValue(ptr + 8 * i + 4, 'i32')]);
    }
    return out;
  }

  // Submit through the engine and record the trades it produced.
  function submit(side, price, qty, ioc, fromUser) {
    const id = api.submit(side, price, qty, ioc);
    processed++;
    if (!id) return { id: 0 };
    const n = api.tradeCount();
    const fills = [];
    for (let i = 0; i < n; i++) {
      const maker = api.tradeMaker(i);
      const p = api.tradePrice(i);
      const q = api.tradeQty(i);
      fills.push([p, q]);
      trades++;
      tape.unshift({ p, q, side, you: fromUser || mine.has(maker), t: new Date() });
      lastTradePrice = p;
      if (mine.has(maker)) {
        const open = api.openQty(maker);
        if (!fromUser) {
          setResult(
            `Your order #${maker} traded ${fmtInt(q)} at ${p}${open ? `; ${fmtInt(open)} still resting.` : ' and is now fully filled.'}`,
            true,
          );
        }
        if (open === 0) mine.delete(maker);
      }
    }
    if (tape.length > 60) tape.length = 60;
    const resting = api.resting();
    dirty = true;
    return { id, filled: api.filled(), resting, fills };
  }

  // ── Simulated market ─────────────────────────────────────────────────
  let simOn = true;
  let rate = 100;
  let carry = 0;
  const rnd = (n) => Math.floor(Math.random() * n);

  function simOrder() {
    fair += (Math.random() - 0.5) * 0.35;
    fair = Math.min(MAX - 500, Math.max(MIN + 500, fair));
    const r = Math.random();
    if (r < 0.6 || simIds.length < 200) {
      const side = Math.random() < 0.5 ? 0 : 1;
      const off = 1 + Math.floor(-Math.log(1 - Math.random()) * 4); // most orders near the touch
      const price = side === 0 ? Math.round(fair) - off : Math.round(fair) + off;
      const res = submit(side, price, 1 + rnd(100), 0, false);
      if (res.resting) simIds.push(res.id);
      if (simIds.length > 3000) api.cancel(simIds.shift());
    } else if (r < 0.85) {
      const k = rnd(simIds.length);
      api.cancel(simIds[k]);
      simIds[k] = simIds[simIds.length - 1];
      simIds.pop();
      processed++;
      dirty = true;
    } else {
      const side = Math.random() < 0.5 ? 0 : 1;
      const bb = api.bestBid();
      const ba = api.bestAsk();
      const price = side === 0 ? (ba || Math.round(fair)) + 2 : (bb || Math.round(fair)) - 2;
      submit(side, Math.min(MAX, Math.max(MIN, price)), 10 + rnd(120), 1, false);
    }
  }

  // Seed a book so the page starts with something to look at.
  for (let i = 0; i < 600; i++) simOrder();

  let lastFrame = performance.now();
  let lastRender = 0;
  function frame(now) {
    if (simOn) {
      carry += (rate * (now - lastFrame)) / 1000;
      const n = Math.min(Math.floor(carry), 2000);
      carry -= n;
      for (let i = 0; i < n; i++) simOrder();
    }
    lastFrame = now;
    if (dirty && now - lastRender > 100) {
      lastRender = now; // ~10 redraws/s keeps prices readable
      render();
    }
    requestAnimationFrame(frame);
  }

  // ── Rendering ────────────────────────────────────────────────────────
  const minePrices = () => {
    const s = new Set();
    for (const [id, o] of mine) if (api.openQty(id) > 0) s.add(`${o.side}:${o.price}`);
    return s;
  };

  // Fixed pools of row elements, updated in place. Rebuilding rows every frame
  // would detach the element under the cursor and swallow clicks.
  function makeRows(el, cls) {
    const rows = [];
    for (let i = 0; i < LEVELS; i++) {
      const row = document.createElement('div');
      row.className = `row ${cls}`;
      row.title = 'Use this price';
      const bar = document.createElement('i');
      bar.className = 'bar';
      const a = document.createElement('span');
      const b = document.createElement('span');
      row.append(bar, a, b);
      el.append(row);
      rows.push({ row, bar, a, b });
    }
    return rows;
  }
  const askRows = makeRows($('asks'), 'ask');
  const bidRows = makeRows($('bids'), 'bid');

  function renderSide(rows, levels, side, maxQ, mark, alignBottom) {
    const offset = alignBottom ? LEVELS - levels.length : 0; // asks: best price sits next to the spread
    rows.forEach((r, i) => {
      const lvl = levels[i - offset];
      if (!lvl) {
        r.row.style.visibility = 'hidden';
        return;
      }
      const [p, q] = lvl;
      r.row.style.visibility = '';
      r.row.dataset.price = p;
      r.row.classList.toggle('mine-level', mark.has(`${side}:${p}`));
      r.bar.style.width = `${Math.max(2, (q / maxQ) * 100)}%`;
      r.a.textContent = p;
      r.b.textContent = fmtInt(q);
    });
  }

  function render() {
    dirty = false;
    const bids = readDepth(0);
    const asks = readDepth(1);
    const maxQ = Math.max(1, ...bids.map((l) => l[1]), ...asks.map((l) => l[1]));
    const mark = minePrices();
    renderSide(askRows, asks.slice().reverse(), 1, maxQ, mark, true);
    renderSide(bidRows, bids, 0, maxQ, mark, false);
    const bb = api.bestBid();
    const ba = api.bestAsk();
    $('spread').textContent = bb && ba ? `Spread ${ba - bb}, mid ${((ba + bb) / 2).toFixed(1)}` : 'One side of the book is empty';
    $('book-stats').textContent = `${fmtInt(api.restingCount())} resting orders, ${fmtInt(trades)} trades, ${fmtInt(processed)} orders processed`;

    const tapeEl = $('tape');
    tapeEl.replaceChildren(
      ...tape.slice(0, 40).map((t, i) => {
        const li = document.createElement('li');
        const next = tape[i + 1];
        const dir = next ? (t.p > next.p ? 'up' : t.p < next.p ? 'down' : '') : '';
        li.className = `${dir}${t.you ? ' you' : ''}`;
        li.title = t.you ? 'Your order traded' : '';
        const a = document.createElement('span');
        a.textContent = t.p;
        const b = document.createElement('span');
        b.textContent = t.q;
        const c = document.createElement('span');
        c.className = 't';
        c.textContent = t.t.toLocaleTimeString('en-GB');
        li.append(a, b, c);
        return li;
      }),
    );
    renderMine();
    updateButton();
  }

  let mineSig = '';
  function renderMine() {
    const ul = $('mine');
    const sig = [...mine.keys()].map((id) => `${id}:${api.openQty(id)}`).join(',');
    if (sig === mineSig) return; // unchanged: keep the same buttons under the cursor
    mineSig = sig;
    const items = [];
    for (const [id, o] of mine) {
      const open = api.openQty(id);
      if (open === 0) {
        mine.delete(id);
        continue;
      }
      const li = document.createElement('li');
      const label = document.createElement('span');
      label.innerHTML = `<span class="${o.side ? 's' : 'b'}">${o.side ? 'Sell' : 'Buy'}</span> ${fmtInt(open)} @ ${o.price} <span class="muted">#${id}</span>`;
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'link-btn';
      btn.textContent = 'Cancel';
      btn.addEventListener('click', () => {
        const ok = api.cancel(id);
        mine.delete(id);
        processed++;
        setResult(ok ? `Order #${id} cancelled.` : `Order #${id} had already filled.`, ok);
        dirty = true;
      });
      li.append(label, btn);
      items.push(li);
    }
    if (!items.length) {
      const li = document.createElement('li');
      li.className = 'muted';
      li.textContent = 'None right now.';
      items.push(li);
    }
    ul.replaceChildren(...items);
  }

  // ── Order form ───────────────────────────────────────────────────────
  let side = 0;
  let ioc = 0;
  const priceEl = $('price');
  const qtyEl = $('qty');
  priceEl.value = api.bestBid() || 10000;

  function updateButton() {
    const btn = $('submit');
    const q = parseInt(qtyEl.value, 10) || 0;
    btn.textContent = `${side ? 'Sell' : 'Buy'} ${fmtInt(q)} @ ${priceEl.value || '–'}`;
    btn.classList.toggle('buy', side === 0);
    btn.classList.toggle('sell', side === 1);
  }

  function setResult(text, ok) {
    const el = $('result');
    el.textContent = text;
    el.className = `result ${ok ? 'ok' : 'err'}`;
  }

  document.querySelectorAll('[data-side]').forEach((b) =>
    b.addEventListener('click', () => {
      side = Number(b.dataset.side);
      document.querySelectorAll('[data-side]').forEach((x) => x.classList.toggle('on', x === b));
      updateButton();
    }),
  );
  document.querySelectorAll('[data-type]').forEach((b) =>
    b.addEventListener('click', () => {
      ioc = Number(b.dataset.type);
      document.querySelectorAll('[data-type]').forEach((x) => x.classList.toggle('on', x === b));
      $('type-hint').textContent = ioc
        ? 'Immediate or cancel fills what it can right now and drops the rest.'
        : 'A limit order fills what it can and leaves the rest in the book.';
    }),
  );
  priceEl.addEventListener('input', updateButton);
  qtyEl.addEventListener('input', updateButton);
  $('ladder').addEventListener('click', (e) => {
    const row = e.target.closest('.row');
    if (!row) return;
    priceEl.value = row.dataset.price;
    updateButton();
    priceEl.focus();
  });

  $('order-form').addEventListener('submit', (e) => {
    e.preventDefault();
    const price = parseInt(priceEl.value, 10);
    const qty = parseInt(qtyEl.value, 10);
    if (!(price >= MIN && price <= MAX)) return setResult(`Price must be between ${MIN} and ${fmtInt(MAX)}.`, false);
    if (!(qty >= 1 && qty <= 100000)) return setResult('Quantity must be between 1 and 100,000.', false);
    const res = submit(side, price, qty, ioc, true);
    if (!res.id) return setResult('The engine rejected the order.', false);
    if (res.resting) mine.set(res.id, { side, price });
    const avg = res.fills.length ? res.fills.reduce((s, [p, q]) => s + p * q, 0) / res.filled : 0;
    let msg;
    if (res.filled === qty) msg = `Order #${res.id} filled ${fmtInt(qty)} at an average of ${avg.toFixed(2)}.`;
    else if (res.filled > 0 && res.resting) msg = `Order #${res.id} filled ${fmtInt(res.filled)} (avg ${avg.toFixed(2)}); ${fmtInt(qty - res.filled)} rests at ${price}.`;
    else if (res.filled > 0) msg = `Order #${res.id} filled ${fmtInt(res.filled)} (avg ${avg.toFixed(2)}); the other ${fmtInt(qty - res.filled)} was cancelled.`;
    else if (res.resting) msg = `Order #${res.id} rests in the book: ${fmtInt(qty)} at ${price}.`;
    else msg = `Nothing matched at ${price}, so the immediate-or-cancel order was dropped.`;
    if (res.fills.length > 1) msg += ` It matched ${res.fills.length} resting orders.`;
    setResult(msg, true);
  });

  // ── Simulation controls ──────────────────────────────────────────────
  $('sim-toggle').addEventListener('click', () => {
    simOn = !simOn;
    $('sim-toggle').textContent = simOn ? 'Pause' : 'Resume';
  });
  $('sim-rate').addEventListener('change', (e) => {
    rate = Number(e.target.value);
  });

  // ── Benchmark ────────────────────────────────────────────────────────
  $('bench-run').addEventListener('click', () => {
    const btn = $('bench-run');
    btn.disabled = true;
    btn.textContent = 'Running…';
    setTimeout(() => {
      api.bench(1000000);
      const add = api.benchResult(0);
      const cancel = api.benchResult(1);
      const ioc = api.benchResult(2);
      const ops = api.benchResult(3);
      const ms = api.benchResult(4);
      $('b-add').textContent = `${add.toFixed(0)} ns`;
      $('b-cancel').textContent = `${cancel.toFixed(0)} ns`;
      $('b-ioc').textContent = `${ioc.toFixed(0)} ns`;
      $('b-tput').textContent = `${((ops / ms) * 1000 / 1e6).toFixed(1)} million operations/s`;
      btn.disabled = false;
      btn.textContent = 'Run again';
    }, 30);
  });

  $('loading').hidden = true;
  $('app').hidden = false;
  $('bench').hidden = false;
  updateButton();
  render();
  requestAnimationFrame(frame);
})();
