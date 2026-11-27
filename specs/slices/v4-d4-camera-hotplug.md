```markdown
# Slice: v4-d4-camera-hotplug

## 目的
采集接缝能看见「设备来了 / 设备走了」，引擎采集泵按 tick 轮询（**不新增引擎线程**），
会话中途拔掉摄像头时干净地失败（报错、不卡死），插回去后被重新打开、帧恢复、计数器能看出来。
重开有退避上界。

## 现状核实（写规格时的树状态，version/v3 @ a2bb408c）

- **已在**：屏幕后端（DXGI Desktop Duplication）有 `DXGI_ERROR_DEVICE_REMOVED` 路径与 `video_capture_restarts` 计数器。
- **缺**：`src/modules/video_capture/` 里**没有**任何设备插拔枚举 / 变更通知（`hotplug` /
  `device_added` / `device_removed` 在树里 0 处）；摄像头类后端（Media Foundation / AVFoundation / V4L2）
  没有「设备移除 / 设备恢复」路径；没有 `video_capture_device_changes` 计数器。

## 范围
要改的（这些是 agent 会去改的文件）：
- `src/modules/video_capture/**`（枚举 / 变更通知 / 后端移除与恢复路径）
- `src/engine/src/engine_media.cpp`（采集泵按 tick 轮询变更 —— 只接线，**不新增线程**）
- `tests/acceptance/v4-d4-camera-hotplug/acceptance.test.cjs`

只读参考（**不得修改**，agent 只许读）：
- 只读 `src/modules/audio_device/` —— 参照它的设备热插拔与默认设备选择写法

## 需求
1. 采集接缝提供「枚举当前设备」与「自上次以来变了什么（加入 / 移除）」；
2. 引擎采集泵在 tick 里消费这个变更（不新增线程）；
3. 重建有退避上界，`video_capture_restarts` 能看出尝试次数；
4. `video_capture_device_changes` 统计设备变更次数；
5. 摄像头类后端补上设备移除 / 恢复路径（三平台各一条）；
6. 会话中途拔掉摄像头 → 干净失败（报出错误，不卡死）；
7. 插回后重新打开、帧恢复，两个计数器都能看出来。

## 验收标准
1. 变更通知存在，且能在单测里用假后端制造「移除 → 加入」被接缝看见。
2. 采集泵在 tick 里消费变更（单测：驱动一次 tick，断言消费了一次变更），**不**新增线程（代码里没有新 `std::thread`）。
3. 重建退避有上界（单测：连续制造移除，断言在退避窗口内的重启次数受限）。
4. `video_capture_restarts` / `video_capture_device_changes` 在拔插时各自递增（单测断言两者**分别**变化）。
5. MF / AVFoundation / V4L2 三后端各有移除与恢复路径（静态断言：三处实现都在，且都不为空）。
6. 拔掉 → 错误回调被调用一次、引擎不卡（单测用假后端模拟）。
7. 插回 → 重新打开成功、帧恢复（单测：第二次 open 成功且首帧到达）。

## 风险
1. 三平台行为不一致 → 各自单测 + 静态断言三后端都在。
2. 退避过长让恢复变慢 → 退避只加「上界」，不缩短首轮重建。
```
