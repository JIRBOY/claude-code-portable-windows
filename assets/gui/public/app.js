'use strict';

marked.setOptions({ gfm: true, breaks: true });

const $ = sel => document.querySelector(sel);
const messagesEl = $('#messages');
const inputEl = $('#input');
const sendBtn = $('#send');
const stopBtn = $('#stop');
const sessionListEl = $('#session-list');
const titleEl = $('#session-title');
const metaEl = $('#session-meta');
const statusPill = $('#status-pill');
const statusText = $('#status-text');
const connectionEl = $('#connection');
const connectionTextEl = $('#connection-text');
const workerCountEl = $('#worker-count');
const costHintEl = $('#cost-hint');
const sidebarEl = $('#sidebar');
const modalEl = $('#modal');
const toastEl = $('#toast');

const state = {
  sessions: [],
  current: null,
  config: { defaultCwd: '', recentCwds: [] },
  blocks: new Map(),
  toolIndex: new Map(),
  streaming: false,
  autoScroll: true,
  lastAssistantEl: null
};

/* ---------- 工具函数 ---------- */

function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

function renderMarkdown(text) {
  const html = marked.parse(text || '');
  return DOMPurify.sanitize(html, { USE_PROFILES: { html: true } });
}

function enhanceMarkdown(container) {
  container.querySelectorAll('a').forEach(a => {
    a.target = '_blank';
    a.rel = 'noopener noreferrer';
  });
  container.querySelectorAll('pre').forEach(pre => {
    if (pre.querySelector('.copy-code')) return;
    const btn = document.createElement('button');
    btn.className = 'copy-code';
    btn.textContent = '复制';
    btn.addEventListener('click', async e => {
      e.preventDefault();
      const code = pre.querySelector('code');
      const text = code ? code.innerText : pre.innerText;
      try {
        await navigator.clipboard.writeText(text);
        btn.textContent = '已复制';
        setTimeout(() => { btn.textContent = '复制'; }, 1200);
      } catch {
        btn.textContent = '失败';
      }
    });
    pre.appendChild(btn);
  });
}

function toast(message) {
  toastEl.textContent = message;
  toastEl.hidden = false;
  clearTimeout(toast._timer);
  toast._timer = setTimeout(() => { toastEl.hidden = true; }, 3200);
}

async function api(path, options) {
  const res = await fetch(path, options);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
  return data;
}

function relativeTime(iso) {
  if (!iso) return '';
  const diff = Date.now() - new Date(iso).getTime();
  if (diff < 60000) return '刚刚';
  if (diff < 3600000) return Math.floor(diff / 60000) + ' 分钟前';
  if (diff < 86400000) return Math.floor(diff / 3600000) + ' 小时前';
  return new Date(iso).toLocaleDateString('zh-CN', { month: 'numeric', day: 'numeric' });
}

function shortPath(p) {
  if (!p) return '';
  return p.replace(/^[A-Za-z]:\\Users\\[^\\]+/, '~').replace(/\\/g, ' / ');
}

/* ---------- 侧栏 ---------- */

function renderSidebar() {
  sessionListEl.innerHTML = '';
  if (!state.sessions.length) {
    const empty = document.createElement('div');
    empty.style.cssText = 'padding:14px 10px;color:var(--faint);font-size:12.5px;';
    empty.textContent = '还没有会话';
    sessionListEl.appendChild(empty);
    return;
  }
  for (const s of state.sessions) {
    const item = document.createElement('div');
    item.className = 'session-item' + (state.current && state.current.id === s.id ? ' active' : '');
    item.innerHTML =
      '<div class="s-title">' + escapeHtml(s.title || '新会话') + '</div>' +
      '<div class="s-sub">' + escapeHtml(shortPath(s.cwd)) + ' · ' + relativeTime(s.updatedAt) + '</div>' +
      '<button class="s-del" title="删除会话">×</button>';
    item.addEventListener('click', e => {
      if (e.target.closest('.s-del')) return;
      openSession(s.id);
    });
    item.querySelector('.s-del').addEventListener('click', async e => {
      e.stopPropagation();
      if (!confirm('删除会话「' + (s.title || '新会话') + '」？\n只删除本界面的记录，不删除 Claude 自身的会话文件。')) return;
      try {
        await api('/api/sessions/' + s.id, { method: 'DELETE' });
        state.sessions = state.sessions.filter(x => x.id !== s.id);
        if (state.current && state.current.id === s.id) {
          state.current = null;
          showEmpty();
        }
        renderSidebar();
      } catch (err) { toast(err.message); }
    });
    sessionListEl.appendChild(item);
  }
}

function updateSessionMeta(session) {
  const s = state.sessions.find(x => x.id === session.id);
  if (s) Object.assign(s, session);
  else state.sessions.unshift(session);
  renderSidebar();
}

/* ---------- 消息渲染 ---------- */

function showEmpty() {
  messagesEl.innerHTML =
    '<div class="empty" id="empty">' +
    '<div class="empty-mark">✳</div><h2>开始一段对话</h2>' +
    '<p>选择一个工作目录，Claude 会在该目录下读写文件、运行命令。</p>' +
    '<button class="primary" id="empty-new">新建会话</button></div>';
  $('#empty-new').addEventListener('click', openModal);
  titleEl.textContent = 'Claude Code';
  metaEl.textContent = '选择或新建一个会话';
  costHintEl.textContent = '';
  setStatus(null);
}

function scrollToBottom(force) {
  if (!force && !state.autoScroll) return;
  messagesEl.scrollTop = messagesEl.scrollHeight;
}

function clearMessages() {
  messagesEl.innerHTML = '';
  state.blocks.clear();
  state.toolIndex.clear();
  state.lastAssistantEl = null;
}

function clearEmptyState() {
  const empty = messagesEl.querySelector('.empty');
  if (empty) empty.remove();
}

function appendUserMessage(text) {
  clearEmptyState();
  const el = document.createElement('div');
  el.className = 'msg user';
  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  bubble.textContent = text;
  el.appendChild(bubble);
  messagesEl.appendChild(el);
  state.lastAssistantEl = null;
  scrollToBottom();
}

function ensureAssistantMessage(messageId) {
  clearEmptyState();
  let el = messagesEl.querySelector('[data-mid="' + CSS.escape(messageId) + '"]');
  if (el) return el;
  el = document.createElement('div');
  el.className = 'msg assistant';
  el.dataset.mid = messageId;
  const prev = messagesEl.lastElementChild;
  if (prev && prev.classList.contains('msg') && prev.classList.contains('assistant')) {
    el.classList.add('continued');
  }
  el.innerHTML = '<div class="avatar">✳</div><div class="body"><div class="blocks"></div></div>';
  messagesEl.appendChild(el);
  state.lastAssistantEl = el;
  if (!state.blocks.has(messageId)) state.blocks.set(messageId, new Map());
  scrollToBottom();
  return el;
}

function createTextBlock(bd) {
  const wrap = document.createElement('div');
  wrap.className = 'block text';
  const md = document.createElement('div');
  md.className = 'md';
  wrap.appendChild(md);
  bd.el = wrap;
  bd.mdEl = md;
  return wrap;
}

function createThinkingBlock(bd) {
  const wrap = document.createElement('details');
  wrap.className = 'thinking';
  const summary = document.createElement('summary');
  summary.textContent = '思考过程';
  const body = document.createElement('div');
  body.className = 'thinking-body';
  wrap.appendChild(summary);
  wrap.appendChild(body);
  bd.el = wrap;
  bd.bodyEl = body;
  return wrap;
}

function toolSummary(bd) {
  const i = bd.input || {};
  if (bd.name === 'Bash') return i.command || i.description || '';
  if (['Read', 'Write', 'Edit', 'NotebookEdit'].includes(bd.name)) return i.file_path || '';
  if (bd.name === 'Grep' || bd.name === 'Glob') return i.pattern || '';
  if (bd.name === 'WebFetch') return i.url || '';
  if (bd.name === 'WebSearch') return i.query || '';
  if (bd.name === 'Task') return i.description || '';
  if (bd.name === 'Skill') return i.skill || '';
  const s = JSON.stringify(i || {});
  return s === '{}' ? '' : s.slice(0, 90);
}

function createToolBlock(bd) {
  const wrap = document.createElement('div');
  wrap.className = 'tool';
  if (bd.id) wrap.dataset.toolId = bd.id;

  const head = document.createElement('div');
  head.className = 'tool-head';
  head.innerHTML =
    '<span class="tool-caret">▸</span>' +
    '<span class="tool-name">' + escapeHtml(bd.name || 'Tool') + '</span>' +
    '<span class="tool-arg"></span>' +
    '<span class="tool-state">运行中…</span>';

  const body = document.createElement('div');
  body.className = 'tool-body';
  body.innerHTML =
    '<div class="label">参数</div><pre class="tool-input"></pre>' +
    '<div class="label tool-result-label" hidden>结果</div><pre class="tool-result" hidden></pre>';

  wrap.appendChild(head);
  wrap.appendChild(body);

  head.addEventListener('click', () => {
    wrap.classList.toggle('open');
    head.querySelector('.tool-caret').textContent = wrap.classList.contains('open') ? '▾' : '▸';
  });

  bd.el = wrap;
  bd.headEl = head;
  bd.argEl = head.querySelector('.tool-arg');
  bd.stateEl = head.querySelector('.tool-state');
  bd.bodyEl = body;
  bd.inputEl = body.querySelector('.tool-input');
  bd.resultLabelEl = body.querySelector('.tool-result-label');
  bd.resultEl = body.querySelector('.tool-result');

  updateToolBlock(bd);
  return wrap;
}

function updateToolBlock(bd) {
  if (!bd.argEl) return;
  bd.argEl.textContent = toolSummary(bd);
  bd.argEl.title = bd.argEl.textContent;
  bd.inputEl.textContent = JSON.stringify(bd.input || {}, null, 2);
  if (bd.result !== undefined && bd.result !== null && bd.result !== '') {
    bd.resultLabelEl.hidden = false;
    bd.resultEl.hidden = false;
    bd.resultEl.textContent = String(bd.result);
    bd.resultEl.classList.toggle('err', !!bd.isError);
    bd.stateEl.textContent = bd.isError ? '失败' : '完成';
    bd.stateEl.classList.toggle('error', !!bd.isError);
  } else {
    bd.stateEl.textContent = '运行中…';
    bd.stateEl.classList.remove('error');
  }
}

function renderBlock(messageId, index, bd) {
  const msgEl = ensureAssistantMessage(messageId);
  const container = msgEl.querySelector('.blocks');
  let el;
  if (bd.type === 'text') el = createTextBlock(bd);
  else if (bd.type === 'thinking') el = createThinkingBlock(bd);
  else if (bd.type === 'tool_use') el = createToolBlock(bd);
  else return;

  const existing = container.querySelector('[data-block-index="' + index + '"]');
  if (existing) existing.replaceWith(el);
  else container.appendChild(el);
  el.dataset.blockIndex = index;

  if (bd.type === 'text') setTextContent(bd, bd.text || '');
  if (bd.type === 'thinking') bd.bodyEl.textContent = bd.thinking || '';
  if (bd.type === 'tool_use' && bd.id) state.toolIndex.set(bd.id, bd);
  scrollToBottom();
}

const dirtyText = new Map();
let renderTimer = null;
function scheduleTextRender(bd) {
  dirtyText.set(bd.el, bd);
  if (renderTimer) return;
  renderTimer = setTimeout(() => {
    renderTimer = null;
    for (const [el, data] of dirtyText) {
      if (!el.isConnected) continue;
      setTextContent(data, data.text || '');
    }
    dirtyText.clear();
  }, 70);
}

function setTextContent(bd, text) {
  if (!bd.mdEl) return;
  bd.mdEl.innerHTML = renderMarkdown(text);
  enhanceMarkdown(bd.mdEl);
}

function setStatus(text) {
  if (!text) { statusPill.hidden = true; return; }
  statusPill.hidden = false;
  statusText.textContent = text;
}

function setStreaming(on) {
  state.streaming = on;
  stopBtn.hidden = !on;
  sendBtn.hidden = on;
  if (!on) setStatus(null);
}

/* ---------- 会话 ---------- */

async function openSession(id) {
  try {
    const data = await api('/api/sessions/' + id);
    state.current = data.session;
    titleEl.textContent = data.session.title || '新会话';
    metaEl.textContent = shortPath(data.session.cwd) + (data.session.model ? ' · ' + data.session.model : '');
    costHintEl.textContent = data.session.lastResult && data.session.lastResult.costUsd
      ? '$' + Number(data.session.lastResult.costUsd).toFixed(4)
      : '';
    clearMessages();
    const transcript = data.session.transcript || [];
    for (const entry of transcript) {
      if (entry.type === 'user') appendUserMessage(entry.text);
      else if (entry.type === 'assistant') {
        state.blocks.set(entry.messageId, new Map());
        entry.blocks.forEach((b, i) => {
          const bd = Object.assign({}, b);
          state.blocks.get(entry.messageId).set(i, bd);
          renderBlock(entry.messageId, i, bd);
        });
      }
    }
    if (!transcript.length) {
      messagesEl.innerHTML =
        '<div class="empty" style="padding-top:9vh">' +
        '<div class="empty-mark">✳</div><h2>对话已就绪</h2>' +
        '<p>' + escapeHtml(shortPath(data.session.cwd)) + '</p></div>';
    }
    state.autoScroll = true;
    scrollToBottom(true);
    renderSidebar();
    setStreaming(!!data.session.busy);
    if (data.session.busy) setStatus('处理中');
    inputEl.focus();
  } catch (err) {
    toast(err.message);
  }
}

async function createSession(cwd) {
  const data = await api('/api/sessions', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ cwd })
  });
  state.sessions.unshift(data.session);
  renderSidebar();
  closeModal();
  await openSession(data.session.id);
}

/* ---------- 发送 ---------- */

async function sendMessage() {
  const text = inputEl.value.trim();
  if (!text || !state.current || state.streaming) return;
  inputEl.value = '';
  resizeInput();
  state.autoScroll = true;
  setStreaming(true);
  setStatus('思考中');
  try {
    await api('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sessionId: state.current.id, text })
    });
  } catch (err) {
    setStreaming(false);
    toast(err.message);
  }
}

async function stopGeneration() {
  if (!state.current) return;
  setStatus('正在停止…');
  try {
    await api('/api/interrupt', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sessionId: state.current.id })
    });
  } catch (err) { toast(err.message); }
}

/* ---------- SSE ---------- */

function handleEvent(ev) {
  const isCurrent = state.current && ev.sessionId === state.current.id;

  if (ev.type === 'session_created' || ev.type === 'session_updated') {
    updateSessionMeta(ev.session);
    if (state.current && ev.session && ev.session.id === state.current.id) {
      state.current.title = ev.session.title;
      titleEl.textContent = ev.session.title || '新会话';
    }
    return;
  }
  if (ev.type === 'session_deleted') {
    state.sessions = state.sessions.filter(x => x.id !== ev.sessionId);
    if (state.current && state.current.id === ev.sessionId) { state.current = null; showEmpty(); }
    renderSidebar();
    return;
  }
  if (!isCurrent) return;

  if (ev.type === 'user_message') {
    appendUserMessage(ev.message.text);
    return;
  }

  if (ev.type === 'init') {
    state.current.model = ev.model;
    metaEl.textContent = shortPath(state.current.cwd) + (ev.model ? ' · ' + ev.model : '');
    return;
  }

  if (ev.type === 'status') {
    if (ev.status === 'requesting') setStatus('思考中');
    else if (ev.status === 'forcing-stop') setStatus('正在停止…');
    return;
  }

  if (ev.type === 'assistant_start') {
    ensureAssistantMessage(ev.messageId);
    setStatus('生成中');
    return;
  }

  if (ev.type === 'block_start') {
    const map = state.blocks.get(ev.messageId) || new Map();
    state.blocks.set(ev.messageId, map);
    const bd = Object.assign({}, ev.block, { inputJson: '' });
    map.set(ev.index, bd);
    renderBlock(ev.messageId, ev.index, bd);
    return;
  }

  if (ev.type === 'block_delta') {
    const map = state.blocks.get(ev.messageId);
    const bd = map && map.get(ev.index);
    if (!bd) return;
    if (ev.kind === 'text') { bd.text = (bd.text || '') + ev.text; scheduleTextRender(bd); }
    else if (ev.kind === 'thinking') { bd.thinking = (bd.thinking || '') + ev.text; if (bd.bodyEl) bd.bodyEl.textContent = bd.thinking; }
    else if (ev.kind === 'input_json') { bd.inputJson = (bd.inputJson || '') + ev.text; }
    return;
  }

  if (ev.type === 'block_stop') {
    const map = state.blocks.get(ev.messageId);
    const bd = map && map.get(ev.index);
    if (bd && bd.type === 'tool_use' && bd.inputJson) {
      try { bd.input = JSON.parse(bd.inputJson); } catch {}
      updateToolBlock(bd);
    }
    return;
  }

  if (ev.type === 'message_final') {
    const map = new Map();
    state.blocks.set(ev.messageId, map);
    const msgEl = ensureAssistantMessage(ev.messageId);
    const container = msgEl.querySelector('.blocks');
    container.innerHTML = '';
    ev.blocks.forEach((b, i) => {
      const bd = Object.assign({}, b);
      if (b.type === 'tool_use' && state.toolIndex.has(b.id)) {
        const old = state.toolIndex.get(b.id);
        bd.result = old.result;
        bd.isError = old.isError;
      }
      map.set(i, bd);
      renderBlock(ev.messageId, i, bd);
    });
    scrollToBottom();
    return;
  }

  if (ev.type === 'tool_result') {
    const bd = state.toolIndex.get(ev.toolUseId);
    if (bd) {
      bd.result = ev.content;
      bd.isError = ev.isError;
      updateToolBlock(bd);
    }
    return;
  }

  if (ev.type === 'result') {
    setStreaming(false);
    if (ev.costUsd != null) {
      costHintEl.textContent = '$' + Number(ev.costUsd).toFixed(4) +
        (ev.durationMs ? ' · ' + (ev.durationMs / 1000).toFixed(1) + 's' : '');
    }
    if (ev.subtype !== 'success' && ev.subtype !== 'error_during_execution') {
      // 保留服务端给出的状态
    }
    state.current.lastResult = ev;
    scrollToBottom();
    return;
  }

  if (ev.type === 'worker_exit') {
    setStreaming(false);
    toast('Claude 进程已退出' + (ev.message ? '：' + ev.message : ''));
    return;
  }

  if (ev.type === 'error') {
    setStreaming(false);
    toast(ev.message);
    return;
  }
}

function connectEvents() {
  const es = new EventSource('/api/events');
  es.onopen = () => {
    connectionEl.classList.remove('offline');
    connectionTextEl.textContent = '已连接';
  };
  es.onmessage = e => {
    if (!e.data) return;
    let ev; try { ev = JSON.parse(e.data); } catch { return; }
    handleEvent(ev);
  };
  es.onerror = () => {
    connectionEl.classList.add('offline');
    connectionTextEl.textContent = '重连中…';
  };
}

/* ---------- 弹窗 ---------- */

function openModal() {
  modalEl.hidden = false;
  const cwdInput = $('#cwd-input');
  cwdInput.value = state.current ? state.current.cwd : (state.config.defaultCwd || '');
  const list = $('#cwd-list');
  list.innerHTML = '';
  for (const c of state.config.recentCwds || []) {
    const opt = document.createElement('option');
    opt.value = c;
    list.appendChild(opt);
  }
  const recent = $('#recent-cwds');
  recent.innerHTML = '';
  for (const c of (state.config.recentCwds || []).slice(0, 5)) {
    const btn = document.createElement('button');
    btn.textContent = shortPath(c);
    btn.title = c;
    btn.addEventListener('click', () => { cwdInput.value = c; });
    recent.appendChild(btn);
  }
  cwdInput.focus();
  cwdInput.select();
}

function closeModal() { modalEl.hidden = true; }

/* ---------- 输入框 ---------- */

function resizeInput() {
  inputEl.style.height = 'auto';
  inputEl.style.height = Math.min(inputEl.scrollHeight, 220) + 'px';
}

/* ---------- 初始化 ---------- */

async function boot() {
  try {
    const cfg = await api('/api/config');
    state.config = cfg;
    workerCountEl.textContent = cfg.workers ? cfg.workers + ' 个进程' : '';
  } catch {}

  try {
    const data = await api('/api/sessions');
    state.sessions = data.sessions;
  } catch (err) { toast(err.message); }
  renderSidebar();

  const lastId = localStorage.getItem('cc-gui-last-session');
  if (lastId && state.sessions.some(s => s.id === lastId)) openSession(lastId);
  else showEmpty();

  connectEvents();
}

/* ---------- 事件绑定 ---------- */

sendBtn.addEventListener('click', sendMessage);
stopBtn.addEventListener('click', stopGeneration);
inputEl.addEventListener('input', resizeInput);
inputEl.addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    sendMessage();
  }
});

$('#new-session').addEventListener('click', openModal);
$('#empty-new') && $('#empty-new').addEventListener('click', openModal);
$('#modal-cancel').addEventListener('click', closeModal);
$('#modal-create').addEventListener('click', () => {
  const cwd = $('#cwd-input').value.trim();
  if (!cwd) { toast('请输入工作目录'); return; }
  createSession(cwd).catch(err => toast(err.message));
});
$('#cwd-input').addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.isComposing) {
    e.preventDefault();
    $('#modal-create').click();
  }
});
modalEl.addEventListener('click', e => { if (e.target === modalEl) closeModal(); });
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && !modalEl.hidden) closeModal();
});

$('#collapse-sidebar').addEventListener('click', () => sidebarEl.classList.add('collapsed'));
$('#open-sidebar').addEventListener('click', () => sidebarEl.classList.remove('collapsed'));

messagesEl.addEventListener('scroll', () => {
  state.autoScroll = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight < 140;
});

const origOpen = openSession;
openSession = async function (id) {
  localStorage.setItem('cc-gui-last-session', id);
  return origOpen(id);
};

boot();
