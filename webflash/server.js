/*
 * 极简本地静态服务器（零依赖，只用 Node 内置模块）
 * 作用：让浏览器能通过 http:// 打开同目录的 index.html，
 *       ESP Web Tools 才能读到 manifest.json 和固件 bin。
 * 启动：node server.js        （或在 Windows 上双击「启动烧录服务.cmd」）
 */
const http = require('http');
const fs = require('fs');
const path = require('path');
const url = require('url');
const { exec } = require('child_process');

const ROOT = __dirname;
const PORT = Number(process.argv[2]) || 8930;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.css':  'text/css; charset=utf-8',
  '.bin':  'application/octet-stream',
  '.png':  'image/png',
  '.jpg':  'image/jpeg',
  '.svg':  'image/svg+xml',
  '.ico':  'image/x-icon',
  '.txt':  'text/plain; charset=utf-8',
  '.md':   'text/plain; charset=utf-8',
};

const server = http.createServer((req, res) => {
  let p;
  try {
    p = decodeURIComponent(url.parse(req.url).pathname);
  } catch (e) {
    res.writeHead(400); res.end('bad request'); return;
  }
  if (p === '/' || p === '') p = '/index.html';

  const file = path.resolve(ROOT, '.' + p);
  if (!file.startsWith(path.resolve(ROOT))) {      // 防目录穿越
    res.writeHead(403); res.end('forbidden'); return;
  }

  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('404 Not Found: ' + p);
      return;
    }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Cache-Control': 'no-cache',
      'Access-Control-Allow-Origin': '*',
    });
    res.end(data);
  });
});

server.on('error', (e) => {
  if (e.code === 'EADDRINUSE') {
    console.log('\n[提示] 端口 ' + PORT + ' 已被占用，可能服务已经在运行了。');
    console.log('直接访问： http://localhost:' + PORT + '/\n');
  } else {
    console.error('服务启动失败：', e.message);
  }
  process.exit(1);
});

server.listen(PORT, '127.0.0.1', () => {
  const addr = 'http://localhost:' + PORT + '/';
  console.log('=============================================');
  console.log('  烧录页已启动');
  console.log('  请在浏览器里打开： ' + addr);
  console.log('  （用 Chrome 或 Edge，按 Ctrl+C 关闭服务）');
  console.log('=============================================');
  const cmd = process.platform === 'win32'
    ? 'start "" "' + addr + '"'
    : (process.platform === 'darwin' ? 'open "' + addr + '"' : 'xdg-open "' + addr + '"');
  exec(cmd, () => {});
});
