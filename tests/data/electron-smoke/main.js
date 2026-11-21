// App thử P1.6 (scripts/realapp-trim.sh): mở cửa sổ ẩn, renderer Chromium dựng
// DOM tiếng Việt và tính 6*7, đọc gói từ node_modules, ghi JSON rồi thoát.
// Kết quả trước và sau khi gọt phải giống hệt nhau.
const { app, BrowserWindow } = require('electron');
const fs = require('fs');
const dep = require('yui-dep');

const deadline = setTimeout(() => {
  console.error('yui-smoke: quá 60 s');
  app.exit(3);
}, 60000);

app.disableHardwareAcceleration();
app.whenReady().then(async () => {
  const win = new BrowserWindow({ show: false, width: 640, height: 480 });
  const html = '<!doctype html><html lang="vi"><meta charset="utf-8"><body><h1>Xin chào YuiVieOS</h1></body></html>';
  await win.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
  const text = await win.webContents.executeJavaScript(
    'document.querySelector("h1").textContent + "|" + (6 * 7)');
  const result = {
    electron: process.versions.electron,
    chrome: process.versions.chrome,
    dep: dep.answer(),
    text,
    locale: app.getLocale(),
  };
  fs.writeFileSync(process.env.YUI_SMOKE_OUT, JSON.stringify(result));
  clearTimeout(deadline);
  app.quit();
}).catch((e) => {
  console.error('yui-smoke:', e);
  app.exit(4);
});
