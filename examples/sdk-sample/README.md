# sdk-sample

一个 **≤200 行**的 SDK 样例：两个进程，同一台机器，跑完一通话（音频 + 视频 + 控制消息），信令复用 `examples/remote-desktop/signalling.py`，媒体走 TURN 中继。宿主只写信令与四个钩子，其余（设备、编解码、BWE、RTX、恢复、渲染、AEC）全是引擎的。

## 怎么跑

样例是**独立工程**，在安装树上编译（spec §五）：

```bash
# 先造一个安装树（N9 的形状），再指向它
cmake --install build --config Debug --prefix build/_install/windows-x86_64
cmake -S examples/sdk-sample -B build/sdk-sample \
  -DCMAKE_PREFIX_PATH=build/_install/windows-x86_64
cmake --build build/sdk-sample --config Debug
```

两个进程分别跑 caller / callee：

```bash
build/sdk-sample/Debug/sdk-sample.exe --role caller --turn-host 127.0.0.1 \
  --turn-port 5349 --turn-user user --turn-pass pass
build/sdk-sample/Debug/sdk-sample.exe --role callee --turn-host 127.0.0.1 \
  --turn-port 5349 --turn-user user --turn-pass pass
```

中间要有一个信令桥把两个进程的 stdin/stdout 对起来，还要有一个 TLS-only 的 TURN 中继 —— CI 的验收腿（`tests/acceptance/v1-sdk-sample/acceptance.test.cjs`）把这两件事都做了。

## 需要什么

- 一个 **TLS-only** 的 TURN 服务器（`--turn-host/--turn-port/--turn-user/--turn-pass`）。
- 信令服务：`examples/remote-desktop/signalling.py`（样例只写客户端）。
- 引擎的公开面：`set_on_call_state`（D7）、`set_on_control_message`、`set_on_ice_recovery`（N8）、`EngineConfig::turn_credentials`（N7）。

## 行数预算

预算 **200** 非注释行，10% 漂移余量（上限 220）。每次 CI 用 `tools/check_sample_line_budget.py` 重新量；量的是 `examples/sdk-sample/` 下**全部** `.cpp` / `.hpp`。涨过 200 会打警告，涨过 220 会红。预算声明在 `sample_line_budget.json`。

## 行为

- 10 秒后退出；退出码 0 表示控制消息交换完成。
- 结束时打一行 `[sdk-sample] summary ...`，验收测试解析它。
