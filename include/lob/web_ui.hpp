#pragma once

// The demo UI's HTML/CSS/JS, embedded as a single constant so lob_server
// is a self-contained binary with no separate static files to ship or
// find at a relative path. It's a plain page (no build step, no
// framework) that polls the JSON API in http_server.cpp / server_main.cpp
// every 400ms -- adequate for a local visual demo of the matching engine,
// not meant to be a low-latency trading UI in its own right.

namespace lob {

constexpr const char* kIndexHtml = R"WEBUI(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Limit Order Book</title>
<style>
  :root {
    --bg: #0b0e14; --panel: #121722; --border: #232a38;
    --text: #e6e9ef; --muted: #8b93a7;
    --bid: #29d398; --bid-bg: rgba(41,211,152,0.14);
    --ask: #ff5d6c; --ask-bg: rgba(255,93,108,0.14);
    --accent: #5b8cff;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    background: var(--bg); color: var(--text); font-size: 14px;
  }
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 14px 20px; border-bottom: 1px solid var(--border);
  }
  header h1 { font-size: 16px; margin: 0; font-weight: 600; }
  header .sub { color: var(--muted); font-size: 12px; }
  input, select, button {
    background: var(--panel); color: var(--text); border: 1px solid var(--border);
    border-radius: 6px; padding: 6px 10px; font-size: 13px;
  }
  button { cursor: pointer; }
  button.primary { background: var(--accent); border-color: var(--accent); color: white; font-weight: 600; }
  button.buy { background: var(--bid); border-color: var(--bid); color: #06281d; font-weight: 600; }
  button.sell { background: var(--ask); border-color: var(--ask); color: #2b0508; font-weight: 600; }
  #symbol { width: 90px; text-transform: uppercase; font-weight: 600; }
  main { display: grid; grid-template-columns: 320px 1fr 280px; gap: 1px; background: var(--border); }
  section { background: var(--bg); padding: 16px; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: 0.06em; color: var(--muted); margin: 0 0 10px; }
  .book-row { display: grid; grid-template-columns: 1fr 1fr; font-variant-numeric: tabular-nums; position: relative; }
  .book-row .price, .book-row .qty { padding: 3px 8px; position: relative; z-index: 1; }
  .book-row .qty { text-align: right; }
  .ask-row .price { color: var(--ask); }
  .bid-row .price { color: var(--bid); }
  .depth-bar { position: absolute; top: 0; bottom: 0; z-index: 0; }
  .ask-row .depth-bar { background: var(--ask-bg); right: 0; }
  .bid-row .depth-bar { background: var(--bid-bg); right: 0; }
  .spread { text-align: center; color: var(--muted); padding: 8px 0; border-top: 1px solid var(--border); border-bottom: 1px solid var(--border); margin: 4px 0; }
  form.order { display: flex; flex-direction: column; gap: 8px; }
  form.order .row { display: flex; gap: 8px; }
  form.order label { color: var(--muted); font-size: 11px; display: block; margin-bottom: 2px; }
  form.order .field { flex: 1; }
  .toggle { display: flex; gap: 6px; }
  .toggle button { flex: 1; opacity: 0.5; }
  .toggle button.active { opacity: 1; }
  .trade { display: flex; justify-content: space-between; padding: 4px 0; border-bottom: 1px solid var(--border); font-variant-numeric: tabular-nums; }
  .trade .up { color: var(--bid); } .trade .down { color: var(--ask); }
  .order-item { display: flex; justify-content: space-between; align-items: center; padding: 6px 0; border-bottom: 1px solid var(--border); }
  .order-item button { padding: 2px 8px; font-size: 11px; }
  .empty { color: var(--muted); font-size: 12px; padding: 8px 0; }
  #status { color: var(--muted); font-size: 11px; }
</style>
</head>
<body>

<header>
  <h1>Limit Order Book</h1>
  <span class="sub">price-time priority matching engine, live</span>
  <span style="flex:1"></span>
  <label for="symbol" class="sub">symbol</label>
  <input id="symbol" value="AAPL">
  <button id="seedBtn">Seed liquidity</button>
  <span id="status"></span>
</header>

<main>
  <section>
    <h2>Place order</h2>
    <form class="order" id="orderForm">
      <div class="toggle" id="sideToggle">
        <button type="button" class="buy active" data-side="buy">BUY</button>
        <button type="button" class="sell" data-side="sell">SELL</button>
      </div>
      <div class="toggle" id="typeToggle">
        <button type="button" class="active" data-type="limit">LIMIT</button>
        <button type="button" data-type="market">MARKET</button>
      </div>
      <div class="row">
        <div class="field">
          <label>price</label>
          <input id="price" type="number" value="10000" style="width:100%">
        </div>
        <div class="field">
          <label>quantity</label>
          <input id="qty" type="number" value="10" style="width:100%">
        </div>
      </div>
      <button class="primary" type="submit">Submit order</button>
    </form>

    <h2 style="margin-top:20px">Your resting orders</h2>
    <div id="myOrders"><div class="empty">none yet</div></div>
  </section>

  <section>
    <h2>Order book</h2>
    <div id="asks"></div>
    <div class="spread" id="spread">—</div>
    <div id="bids"></div>
  </section>

  <section>
    <h2>Trade tape</h2>
    <div id="trades"><div class="empty">no trades yet</div></div>
  </section>
</main>

<script>
const state = { side: 'buy', type: 'limit', myOrders: [] };

function symbol() { return document.getElementById('symbol').value.trim().toUpperCase() || 'AAPL'; }
function setStatus(msg, isError) {
  const el = document.getElementById('status');
  el.textContent = msg;
  el.style.color = isError ? 'var(--ask)' : 'var(--muted)';
}

document.getElementById('sideToggle').addEventListener('click', (e) => {
  const btn = e.target.closest('button'); if (!btn) return;
  state.side = btn.dataset.side;
  for (const b of e.currentTarget.children) b.classList.toggle('active', b === btn);
});
document.getElementById('typeToggle').addEventListener('click', (e) => {
  const btn = e.target.closest('button'); if (!btn) return;
  state.type = btn.dataset.type;
  for (const b of e.currentTarget.children) b.classList.toggle('active', b === btn);
  document.getElementById('price').disabled = (state.type === 'market');
});

document.getElementById('orderForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const sym = symbol();
  const qty = document.getElementById('qty').value;
  const price = document.getElementById('price').value;
  const params = new URLSearchParams({ symbol: sym, side: state.side, type: state.type, qty });
  if (state.type === 'limit') params.set('price', price);
  try {
    const res = await fetch('/api/order', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: params });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || 'order rejected');
    if (state.type === 'limit' && data.unfilled > 0) {
      state.myOrders.push({ symbol: sym, id: data.order_id });
    }
    setStatus(`order #${data.order_id}: ${data.trades.length} fill(s), ${data.unfilled} unfilled`);
    renderMyOrders();
    refresh();
  } catch (err) {
    setStatus(String(err.message || err), true);
  }
});

document.getElementById('seedBtn').addEventListener('click', async () => {
  await fetch('/api/seed', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: new URLSearchParams({ symbol: symbol(), count: '40' }) });
  refresh();
});

async function cancelOrder(sym, id) {
  await fetch('/api/cancel', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: new URLSearchParams({ symbol: sym, order_id: id }) });
  state.myOrders = state.myOrders.filter(o => o.id !== id);
  renderMyOrders();
  refresh();
}

function renderMyOrders() {
  const el = document.getElementById('myOrders');
  const mine = state.myOrders.filter(o => o.symbol === symbol());
  if (mine.length === 0) { el.innerHTML = '<div class="empty">none yet</div>'; return; }
  el.innerHTML = mine.map(o => `
    <div class="order-item">
      <span>#${o.id}</span>
      <button onclick="cancelOrder('${o.symbol}', ${o.id})">cancel</button>
    </div>`).join('');
}

function renderBook(levels, side) {
  if (levels.length === 0) return '<div class="empty">empty</div>';
  const maxQty = Math.max(...levels.map(l => l[1]));
  return levels.map(([price, qty]) => {
    const pct = Math.round((qty / maxQty) * 100);
    return `<div class="book-row ${side}-row">
      <div class="depth-bar" style="width:${pct}%"></div>
      <div class="price">${price}</div>
      <div class="qty">${qty}</div>
    </div>`;
  }).join('');
}

async function refresh() {
  try {
    const sym = symbol();
    const [bookRes, tradesRes] = await Promise.all([
      fetch(`/api/book?symbol=${encodeURIComponent(sym)}&depth=12`),
      fetch(`/api/trades?symbol=${encodeURIComponent(sym)}&limit=25`),
    ]);
    const book = await bookRes.json();
    const trades = await tradesRes.json();

    document.getElementById('asks').innerHTML = renderBook([...book.asks].reverse(), 'ask');
    document.getElementById('bids').innerHTML = renderBook(book.bids, 'bid');

    if (book.asks.length && book.bids.length) {
      const spread = book.asks[0][0] - book.bids[0][0];
      document.getElementById('spread').textContent = `spread ${spread}  (best bid ${book.bids[0][0]} / best ask ${book.asks[0][0]})`;
    } else {
      document.getElementById('spread').textContent = '—';
    }

    const tradesEl = document.getElementById('trades');
    tradesEl.innerHTML = trades.trades.length === 0 ? '<div class="empty">no trades yet</div>' :
      trades.trades.slice().reverse().map(t => `<div class="trade"><span>${t.qty} @ <b>${t.price}</b></span><span class="up">#${t.resting_id}</span><span class="down">#${t.aggressor_id}</span></div>`).join('');

    setStatus(`connected · ${sym}`);
  } catch (err) {
    setStatus('disconnected: ' + err.message, true);
  }
}

renderMyOrders();
refresh();
setInterval(refresh, 400);
</script>
</body>
</html>
)WEBUI";

} // namespace lob
