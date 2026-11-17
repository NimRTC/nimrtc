# Slice: v3-chrome-waterfall

## 目的
把 Chrome WebTransport 的互通页从「能连上」做成「能玩」：新增一个瀑布页，
用 `new WebTransport(url)` 建连（**不是** `createDataChannel`），
收一路视频、显示「输入 → 帧呈现」的瀑布时序，让 M2 的处刑台有一个浏览器侧的对照组。

对应 `docs/plan/ai-game-demo-plan.md` §8 M2 第二条，
以及 §4.1 链路图里「客户端渲染（d3d11 / Chrome）」这一支。

## 范围
- interop/chrome/demo_waterfall.html
- interop/chrome/test_chrome_webtransport.html
- tools/rd_lan.py
- tests/acceptance/v3-chrome-waterfall/acceptance.test.cjs

## 需求
1. 新增 `interop/chrome/demo_waterfall.html`：用 `new WebTransport(url)` 建连，
   不用 `createDataChannel`（§8 M2 明确点出这一条区别）。
2. 页面接收视频流并呈现，同时把「收到帧的时刻」按瀑布图方式画出来。
3. 页面能发输入事件（键盘/鼠标），并把输入时刻标在瀑布图上 ——
   这样「input-to-photon」在浏览器侧可读。
4. 页面把建连结果、帧计数、瀑布统计暴露在页面上（人能直接看，不必翻日志）。
5. 保留 `interop/chrome/test_chrome_webtransport.html` 原有行为，本片是**新增**页面而不是改坏旧页面。
6. `tools/rd_lan.py` 支持 `--viewer-url` 参数，把 verdict 打到 Chrome 页面对应的通道上；
   不传该参数时行为与改动前完全一致。

## 验收标准
1. 新页面上存在 `new WebTransport(` 调用，且**不存在** `createDataChannel` 调用；验证方式：静态检查该页面的 HTML 内容
2. 页面能显示收到的帧计数；验证方式：静态检查页面存在帧计数元素与更新逻辑
3. 页面存在输入事件的发送处理与时刻标注；验证方式：静态检查页面含输入事件处理与瀑布标注代码
4. `interop/chrome/test_chrome_webtransport.html` 未被删改关键逻辑；验证方式：静态检查该文件仍存在且含 `WebTransport` 连接逻辑
5. `tools/rd_lan.py` 新增 `--viewer-url` 参数且缺省时行为不变；验证方式：静态检查参数定义与缺省值
6. 页面在无信令时给出明确的错误提示而不是静默空白；验证方式：静态检查页面含错误提示元素与赋值

## 风险
1. Chrome 的 WebTransport 与 NimRTC 的实现不完全对齐（DTLS 指纹、ALPN）；应对：本片只保证「能建连并出画」，协议细节差异如实报告，不静默降级
2. 浏览器不允许从 `file://` 打开 WebTransport（需要安全上下文）；应对：页面在 `file://` 下明确提示「需要 http(s) 服务」
3. 瀑布图在 4K 屏上帧号看不清；应对：帧号与时刻用等宽字体并保留足够行距
