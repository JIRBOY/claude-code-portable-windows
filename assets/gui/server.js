'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawn, execFile } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const PUBLIC = path.join(__dirname, 'public');
const DATA = path.join(__dirname, 'data');
const SESSIONS_FILE = path.join(DATA, 'sessions.json');
const PORT = Number(process.env.CC_GUI_PORT || 4317);
const HOST = '127.0.0.1';
const MAX_WORKERS = 4;
const IDLE_MS = 30 * 60 * 1000;

fs.mkdirSync(DATA, { recursive: true });
fs.mkdirSync(path.join(DATA, 'sessions'), { recursive: true });

let sessions = [];
try {
  const raw = JSON.parse(fs.readFileSync(SESSIONS_FILE, 'utf8'));
  if (Array.isArray(raw.sessions)) sessions = raw.sessions;
} catch {
  sessions = [];
}

let saveTimer = null;
function saveSessions() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    const tmp = SESSIONS_FILE + '.tmp';
    fs.writeFileSync(tmp, JSON.stringify({ version: 1, sessions }, null, 2), 'utf8');
    fs.renameSync(tmp, SESSIONS_FILE);
  }, 250);
}

const clients = new Set();
function broadcast(payload) {
  const line = 'data: ' + JSON.stringify(payload) + '\n\n';
  for (const res of clients) {
    try { res.write(line); } catch { clients.delete(res); }
  }
}

const workers = new Map();

function sessionById(id) {
  return sessions.find(s => s.id === id);
}

function workerFor(session) {
  return workers.get(session.id);
}

function touch(session) {
  session.updatedAt = new Date().toISOString();
  saveSessions();
}

function pushTranscript(session, entry) {
  session.transcript.push(entry);
  touch(session);
}

function ensureWorker(session) {
  let worker = workers.get(session.id);
  if (worker) {
    worker.lastUsed = Date.now();
    return worker;
  }
  if (workers.size >= MAX_WORKERS) {
    let oldest = null;
    for (const [id, w] of workers) {
      if (!oldest || w.lastUsed < oldest.lastUsed) oldest = { id, w };
    }
    if (oldest) closeWorker(oldest.id);
  }

  const args = [
    '-p', '--input-format', 'stream-json', '--output-format', 'stream-json',
    '--verbose', '--include-partial-messages'
  ];
  if (session.claudeSessionId) args.push('--resume', session.claudeSessionId);

  const proc = spawn(path.join(ROOT, 'scc.exe'), args, {
    cwd: session.cwd,
    stdio: ['pipe', 'pipe', 'pipe'],
    windowsHide: true
  });

  worker = {
    proc,
    buffer: '',
    sessionId: session.id,
    busy: false,
    lastUsed: Date.now(),
    currentMessageId: null,
    blocksByMessage: new Map(),
    interruptTimer: null,
    stderr: ''
  };
  workers.set(session.id, worker);

  proc.stdout.setEncoding('utf8');
  proc.stdout.on('data', chunk => {
    worker.buffer += chunk;
    let idx;
    while ((idx = worker.buffer.indexOf('\n')) >= 0) {
      const line = worker.buffer.slice(0, idx).trim();
      worker.buffer = worker.buffer.slice(idx + 1);
      if (!line) continue;
      let event;
      try { event = JSON.parse(line); } catch { continue; }
      handleClaudeEvent(session, worker, event);
    }
  });

  proc.stderr.setEncoding('utf8');
  proc.stderr.on('data', chunk => {
    const text = chunk.trim();
    if (!text || text.includes('unrecognized_model')) return;
    worker.stderr = (worker.stderr + '\n' + text).slice(-4000);
  });

  proc.on('exit', code => {
    if (workers.get(session.id) !== worker) return;
    workers.delete(session.id);
    workersRecover(session, code, worker.stderr);
  });

  proc.on('error', err => {
    broadcast({ type: 'error', sessionId: session.id, message: '启动 Claude 失败：' + err.message });
  });

  return worker;
}

function workersRecover(session, code, stderr) {
  if (workerRecoveryGuard.has(session.id)) return;
  workerRecoveryGuard.add(session.id);
  setTimeout(() => workerRecoveryGuard.delete(session.id), 1000);
  broadcast({
    type: 'worker_exit',
    sessionId: session.id,
    code,
    message: stderr ? stderr.split('\n').filter(Boolean).slice(-3).join(' | ') : ''
  });
}

const workerRecoveryGuard = new Set();

function closeWorker(sessionId) {
  const worker = workers.get(sessionId);
  if (!worker) return;
  workers.delete(sessionId);
  try { worker.proc.stdin.end(); } catch {}
  const pid = worker.proc.pid;
  setTimeout(() => {
    try {
      if (!worker.proc.killed) {
        execFile('taskkill', ['/PID', String(pid), '/T', '/F'], { windowsHide: true }, () => {});
      }
    } catch {}
  }, 1500);
}

function handleClaudeEvent(session, worker, event) {
  const type = event.type;
  if (type === 'system' && event.subtype === 'init') {
    session.claudeSessionId = event.session_id;
    session.model = event.model;
    session.permissionMode = event.permissionMode;
    session.skills = Array.isArray(event.skills) ? event.skills.length : session.skills;
    touch(session);
    broadcast({
      type: 'init',
      sessionId: session.id,
      claudeSessionId: event.session_id,
      model: event.model,
      permissionMode: event.permissionMode,
      cwd: event.cwd
    });
    return;
  }

  if (type === 'system' && event.subtype === 'status') {
    broadcast({ type: 'status', sessionId: session.id, status: event.status });
    return;
  }

  if (type === 'stream_event') {
    const ev = event.event;
    if (!ev) return;
    if (ev.type === 'message_start') {
      worker.currentMessageId = ev.message.id;
      broadcast({
        type: 'assistant_start',
        sessionId: session.id,
        messageId: ev.message.id,
        model: ev.message.model
      });
      return;
    }
    const messageId = event.api_message_id || worker.currentMessageId;
    if (!messageId) return;

    if (ev.type === 'content_block_start') {
      const cb = ev.content_block || {};
      const block = { type: cb.type };
      if (cb.type === 'text') block.text = cb.text || '';
      if (cb.type === 'thinking') block.thinking = cb.thinking || '';
      if (cb.type === 'tool_use') {
        block.id = cb.id;
        block.name = cb.name;
        block.input = cb.input || {};
        block.inputJson = '';
      }
      broadcast({
        type: 'block_start',
        sessionId: session.id,
        messageId,
        index: ev.index,
        block
      });
      return;
    }

    if (ev.type === 'content_block_delta') {
      const d = ev.delta || {};
      if (d.type === 'text_delta') {
        broadcast({ type: 'block_delta', sessionId: session.id, messageId, index: ev.index, kind: 'text', text: d.text || '' });
      } else if (d.type === 'thinking_delta') {
        broadcast({ type: 'block_delta', sessionId: session.id, messageId, index: ev.index, kind: 'thinking', text: d.thinking || '' });
      } else if (d.type === 'input_json_delta') {
        broadcast({ type: 'block_delta', sessionId: session.id, messageId, index: ev.index, kind: 'input_json', text: d.partial_json || '' });
      }
      return;
    }

    if (ev.type === 'content_block_stop') {
      broadcast({ type: 'block_stop', sessionId: session.id, messageId, index: ev.index });
      return;
    }
    return;
  }

  if (type === 'assistant') {
    const message = event.message;
    if (!message || !Array.isArray(message.content)) return;
    const blocks = message.content.map(c => {
      if (c.type === 'text') return { type: 'text', text: c.text || '' };
      if (c.type === 'thinking') return { type: 'thinking', thinking: c.thinking || '' };
      if (c.type === 'tool_use') return { type: 'tool_use', id: c.id, name: c.name, input: c.input || {} };
      return { type: c.type, raw: c };
    });
    const entry = {
      type: 'assistant',
      messageId: message.id,
      model: message.model,
      blocks,
      at: new Date().toISOString()
    };
    const existing = session.transcript.find(x => x.type === 'assistant' && x.messageId === message.id);
    if (existing) {
      existing.blocks = blocks;
      touch(session);
    } else {
      pushTranscript(session, entry);
    }
    broadcast({ type: 'message_final', sessionId: session.id, messageId: message.id, blocks, usage: message.usage });
    return;
  }

  if (type === 'user') {
    const content = event.message && event.message.content;
    if (!Array.isArray(content)) return;
    for (const c of content) {
      if (c.type !== 'tool_result') continue;
      const result = {
        type: 'tool_result',
        toolUseId: c.tool_use_id,
        isError: !!c.is_error,
        content: normalizeToolResult(c.content),
        at: new Date().toISOString()
      };
      session.transcript.push(result);
      for (const entry of session.transcript) {
        if (entry.type === 'assistant') {
          for (const b of entry.blocks) {
            if (b.type === 'tool_use' && b.id === c.tool_use_id) {
              b.result = result.content;
              b.isError = result.isError;
            }
          }
        }
      }
      touch(session);
      broadcast({ type: 'tool_result', sessionId: session.id, ...result });
    }
    return;
  }

  if (type === 'result') {
    worker.busy = false;
    if (worker.interruptTimer) { clearTimeout(worker.interruptTimer); worker.interruptTimer = null; }
    const payload = {
      type: 'result',
      sessionId: session.id,
      subtype: event.subtype || 'success',
      costUsd: event.total_cost_usd,
      durationMs: event.duration_ms,
      usage: event.usage
    };
    session.lastResult = payload;
    touch(session);
    broadcast(payload);
    return;
  }

  if (type === 'control_response') {
    broadcast({ type: 'control_response', sessionId: session.id, response: event.response });
    return;
  }
}

function normalizeToolResult(content) {
  if (typeof content === 'string') return content;
  if (Array.isArray(content)) {
    return content.map(c => {
      if (c && typeof c === 'object') {
        if (c.type === 'text') return c.text || '';
        return JSON.stringify(c);
      }
      return String(c);
    }).join('\n');
  }
  if (content == null) return '';
  try { return JSON.stringify(content, null, 2); } catch { return String(content); }
}

function sendUserMessage(session, text) {
  const worker = ensureWorker(session);
  const message = { type: 'user', at: new Date().toISOString(), text };
  pushTranscript(session, message);
  broadcast({ type: 'user_message', sessionId: session.id, message });
  worker.busy = true;
  worker.lastUsed = Date.now();
  const payload = {
    type: 'user',
    message: { role: 'user', content: [{ type: 'text', text }] }
  };
  worker.proc.stdin.write(JSON.stringify(payload) + '\n');
}

function interruptSession(session) {
  const worker = workers.get(session.id);
  if (!worker) return false;
  const requestId = 'int_' + crypto.randomBytes(4).toString('hex');
  try {
    worker.proc.stdin.write(JSON.stringify({
      type: 'control_request',
      request_id: requestId,
      request: { subtype: 'interrupt' }
    }) + '\n');
  } catch {
    return false;
  }
  if (worker.interruptTimer) clearTimeout(worker.interruptTimer);
  worker.interruptTimer = setTimeout(() => {
    if (worker.busy) {
      broadcast({ type: 'status', sessionId: session.id, status: 'forcing-stop' });
      closeWorker(session.id);
      broadcast({ type: 'result', sessionId: session.id, subtype: 'interrupted', interrupted: true });
    }
  }, 5000);
  return true;
}

function createSession(cwd, title) {
  const dir = path.resolve(cwd || ROOT);
  if (!fs.existsSync(dir) || !fs.statSync(dir).isDirectory()) {
    const err = new Error('工作目录不存在：' + dir);
    err.statusCode = 400;
    throw err;
  }
  const now = new Date().toISOString();
  const session = {
    id: crypto.randomUUID(),
    title: title || '新会话',
    cwd: dir,
    claudeSessionId: null,
    createdAt: now,
    updatedAt: now,
    transcript: []
  };
  sessions.unshift(session);
  saveSessions();
  return session;
}

function publicSession(session, withTranscript) {
  const out = {
    id: session.id,
    title: session.title,
    cwd: session.cwd,
    claudeSessionId: session.claudeSessionId,
    model: session.model,
    createdAt: session.createdAt,
    updatedAt: session.updatedAt,
    messageCount: session.transcript.filter(x => x.type === 'user').length,
    running: !!workers.get(session.id),
    busy: !!(workers.get(session.id) && workers.get(session.id).busy),
    lastResult: session.lastResult || null
  };
  if (withTranscript) out.transcript = session.transcript;
  return out;
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let data = '';
    req.on('data', c => {
      data += c;
      if (data.length > 10 * 1024 * 1024) { reject(new Error('请求体过大')); req.destroy(); }
    });
    req.on('end', () => {
      if (!data) return resolve({});
      try { resolve(JSON.parse(data)); } catch (e) { reject(new Error('JSON 无效')); }
    });
    req.on('error', reject);
  });
}

function json(res, code, body) {
  const text = JSON.stringify(body);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store'
  });
  res.end(text);
}

const STATIC = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/style.css': ['style.css', 'text/css; charset=utf-8'],
  '/vendor/marked.js': [path.join(__dirname, 'node_modules/marked/lib/marked.umd.js'), 'text/javascript; charset=utf-8'],
  '/vendor/purify.js': [path.join(__dirname, 'node_modules/dompurify/dist/purify.min.js'), 'text/javascript; charset=utf-8']
};

function serveStatic(res, filePath, mime) {
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404); res.end('Not found'); return; }
    res.writeHead(200, { 'Content-Type': mime, 'Cache-Control': 'no-cache' });
    res.end(data);
  });
}

async function route(req, res) {
  const url = new URL(req.url, 'http://' + HOST + ':' + PORT);
  const pathname = decodeURIComponent(url.pathname);

  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Referrer-Policy', 'no-referrer');

  if (req.method === 'GET' && STATIC[pathname]) {
    const [rel, mime] = STATIC[pathname];
    serveStatic(res, path.isAbsolute(rel) ? rel : path.join(PUBLIC, rel), mime);
    return;
  }

  if (req.method === 'GET' && pathname === '/api/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache, no-transform',
      'Connection': 'keep-alive'
    });
    res.write(': connected\n\n');
    clients.add(res);
    const heartbeat = setInterval(() => {
      try { res.write(': ping\n\n'); } catch {}
    }, 25000);
    req.on('close', () => {
      clearInterval(heartbeat);
      clients.delete(res);
    });
    return;
  }

  if (req.method === 'GET' && pathname === '/api/config') {
    json(res, 200, {
      defaultCwd: ROOT,
      recentCwds: [...new Set(sessions.map(s => s.cwd))].slice(0, 10),
      workers: workers.size
    });
    return;
  }

  if (req.method === 'GET' && pathname === '/api/sessions') {
    json(res, 200, { sessions: sessions.map(s => publicSession(s, false)) });
    return;
  }

  const one = pathname.match(/^\/api\/sessions\/([0-9a-f-]+)$/);
  if (one && req.method === 'GET') {
    const session = sessionById(one[1]);
    if (!session) return json(res, 404, { error: '会话不存在' });
    return json(res, 200, { session: publicSession(session, true) });
  }

  if (one && req.method === 'DELETE') {
    const session = sessionById(one[1]);
    if (!session) return json(res, 404, { error: '会话不存在' });
    closeWorker(session.id);
    sessions = sessions.filter(s => s.id !== session.id);
    saveSessions();
    broadcast({ type: 'session_deleted', sessionId: session.id });
    return json(res, 200, { ok: true });
  }

  if (req.method === 'POST' && pathname === '/api/sessions') {
    try {
      const body = await readBody(req);
      const session = createSession(body.cwd, body.title);
      broadcast({ type: 'session_created', session: publicSession(session, false) });
      return json(res, 200, { session: publicSession(session, true) });
    } catch (e) {
      return json(res, e.statusCode || 400, { error: e.message });
    }
  }

  if (req.method === 'POST' && pathname === '/api/chat') {
    try {
      const body = await readBody(req);
      const session = sessionById(body.sessionId);
      if (!session) return json(res, 404, { error: '会话不存在' });
      const text = String(body.text || '').trim();
      if (!text) return json(res, 400, { error: '消息为空' });
      if (!session.title || session.title === '新会话') {
        session.title = text.replace(/\s+/g, ' ').slice(0, 30);
      }
      sendUserMessage(session, text);
      broadcast({ type: 'session_updated', session: publicSession(session, false) });
      return json(res, 200, { ok: true });
    } catch (e) {
      return json(res, 400, { error: e.message });
    }
  }

  if (req.method === 'POST' && pathname === '/api/interrupt') {
    try {
      const body = await readBody(req);
      const session = sessionById(body.sessionId);
      if (!session) return json(res, 404, { error: '会话不存在' });
      return json(res, 200, { ok: interruptSession(session) });
    } catch (e) {
      return json(res, 400, { error: e.message });
    }
  }

  if (req.method === 'POST' && pathname === '/api/shutdown') {
    json(res, 200, { ok: true });
    setTimeout(() => {
      for (const id of [...workers.keys()]) closeWorker(id);
      saveSessions();
      setTimeout(() => process.exit(0), 500);
    }, 100);
    return;
  }

  json(res, 404, { error: '未找到' });
}

const server = http.createServer((req, res) => {
  const origin = req.headers.origin;
  if (origin && origin !== `http://${HOST}:${PORT}` && origin !== `http://localhost:${PORT}`) {
    res.writeHead(403);
    res.end('Forbidden');
    return;
  }
  route(req, res).catch(err => {
    try { json(res, 500, { error: err.message }); } catch {}
  });
});

server.listen(PORT, HOST, () => {
  const url = `http://${HOST}:${PORT}`;
  console.log('');
  console.log('  Claude Code GUI');
  console.log('  ' + url);
  console.log('  工作目录: ' + ROOT);
  console.log('  按 Ctrl+C 结束');
  console.log('');
  saveSessions();
  if (process.env.CC_GUI_OPEN === '1') {
    execFile('cmd', ['/c', 'start', '', url], { windowsHide: true }, () => {});
  }
});

process.on('SIGINT', () => {
  for (const id of [...workers.keys()]) closeWorker(id);
  saveSessions();
  setTimeout(() => process.exit(0), 600);
});

setInterval(() => {
  const now = Date.now();
  for (const [id, w] of workers) {
    if (!w.busy && now - w.lastUsed > IDLE_MS) closeWorker(id);
  }
}, 60000);
