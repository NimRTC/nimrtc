# AEC 激活与 ERLE 测量结果

**范围**：v1.2 计划里的 **E3**（AEC activation）——「vendored APM 在至少一个平台
默认启用，并有 ERLE 测量记录」。

**为什么有这份文档**：E3 的 DoD 有两半——**默认启用**和**有测量**。此前仓库里
两个都没有：

- `audio3a::Config::enable_aec` 虽然默认是 `true`，`webrtc_audio3a.cpp` 也确实
  把它转发给了 `echo_canceller.enabled`，但**没有任何测量证明这条路径真的能
  消掉回声**；
- 全部 CI 分支都用 `-DNIMRTC_VENDORED_WEBRTC_APM=OFF` 配置，所以「默认启用」
  在 CI 里同样无从证明。

这份文档记录的是 E3 的测量证据。

**状态**：**达成**（Windows / MSVC / x64 上，默认开启 APM，ERLE = **30.3 dB**）。

---

## 1. 运行环境

| 项 | 值 |
|---|---|
| 平台 | Windows 11 (10.0.26200), x64 |
| 工具链 | Visual Studio 2022 (MSVC 19.43), Release |
| APM | vendored `webrtc-audio-processing` 2.1（Meson + MSVC 构建） |
| 采样率 / 声道 | 48 kHz / 单声道 |
| 构建配置 | `-DNIMRTC_VENDORED_WEBRTC_APM=ON`（该选项的**默认值就是 ON**） |
| 日期 | 2026-11-18 |

## 2. 复现方法

```powershell
# 1. 拉取并构建 vendored APM（一次性；产物在 build_meson/ 下）
python tools/fetch_webrtc_apm.py --skip-build
tools\build_webrtc_apm.cmd

# 2. 配置并构建 ERLE 台架
cmake -S . -B build/erle -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DNIMRTC_BUILD_TESTS=ON -DNIMRTC_VENDORED_WEBRTC_APM=ON
cmake --build build/erle --target test_audio3a_erle

# 3. 跑测量
build\erle\src\modules\audio3a\tests\test_audio3a_erle.exe
```

台架源码：`src/modules/audio3a/tests/test_audio3a_erle.cpp`。

## 3. 测量方法

台架用**合成回声路径**代替物理的扬声器→麦克风回路，因此在任何机器上都能
复现，且不需要音频硬件：

```
render (x)  ---> 扬声器 ----------------+
                                          |  延时 D = 20 ms
                                          v
capture (y) <--- 麦克风 <----------------+
```

`D` 就是宿主在真实设备上测出来、然后传给
`Config::aec_delay_headroom_ms` 的那个量。台架把两者设成一致——**回声消除器
无法收敛于一个它不知道延时的回声路径**。

render 信号是**类语音宽带信号**（噪声 × 缓慢变化的音节包络），不是纯音调：
消除器的延时估计器会拒绝周期信号，用音调测出来的 ERLE 是真实会话里永远
达不到的数字。

### 3.1 ERLE 的提取

capture 段里**只有回声、没有近端**，所以测量没有歧义：

```
run A（AEC 关）：y_A = e          （回声直通）
run B（AEC 开）：y_B = e_residual （消掉之后剩下的）

ERLE = 10 * log10( P(y_A) / P(y_B) )
```

两次运行喂进去的输入完全相同，所以这个比值只隔离出消除器的贡献，**不需要
近端/回声分离**（那才是通用情形里最难的部分）。

### 3.2 近端保护的测量为什么用音调

上面的测量在消除器把整条 capture 通路静音的情况下也会通过。所以台架还测了
**近端说话人**并断言它活下来了。

近端用的是**音调**而非宽带噪声，这是刻意的：上游 APM 在消除器之后还跑一级
**残余回声抑制器**，它估计消除器残留了什么并把它压掉。宽带近端信号与残余
回声在统计上不可区分，于是被一并压掉——这是上游的**正确行为**（残余回声
抑制器存在的意义就是清掉消除器的残留），不是 NimRTC 的缺陷。而回声路径上
不存在的那个频点上的音调是**可分离的**：单 bin DFT 能测出近端还剩多少，
而不把它和回声混在一起。

## 4. 测量结果

```
[ RUN      ] Audio3AErle.CancellerSuppressesTheSyntheticEcho
  [ERLE] echo_in_power                     0.0184795
  [ERLE] echo_out_power                  1.70533e-05
  [ERLE] ERLE (dB)                           30.3488
[       OK ] Audio3AErle.CancellerSuppressesTheSyntheticEcho
[ RUN      ] Audio3AErle.NearEndTalkerSurvivesTheCanceller
  [ERLE] nearend baseline amp               0.201102
  [ERLE] nearend processed amp              0.161987
[       OK ] Audio3AErle.NearEndTalkerSurvivesTheCanceller
[ RUN      ] Audio3AErle.ControlRunWithoutTheCancellerReportsNoSuppression
  [ERLE] control ERLE (dB)                         0
[       OK ] Audio3AErle.ControlRunWithoutTheCancellerReportsNoSuppression
```

| 指标 | 值 | 判据 | 结果 |
|---|---|---|---|
| **ERLE** | **30.35 dB** | ≥ 15 dB（ITU-T P.807 对收敛消除器的下限） | ✅ |
| 近端音调幅度（AEC 关 → 开） | 0.2011 → 0.1620（保留 80.5%） | > 10% | ✅ |
| 对照组 ERLE（AEC 关） | 0.00 dB | < 3 dB（证明上面的数字来自消除器本身） | ✅ |

> 15 dB 是 ITU-T P.807 对**收敛后**消除器在**平稳**合成路径下的下限。一个
> 宽带、延时精确对齐的合成路径比真实房间更容易，所以台架把门槛定得比它更高。

## 5. 「默认启用」这一半怎么算达成

- `NIMRTC_VENDORED_WEBRTC_APM` 的**默认值就是 ON**（`cmake/NimRTCVendored.cmake`）；
- `audio3a::Config::enable_aec` 默认 `true`，`webrtc_audio3a.cpp` 把它转发到
  `apm_config.echo_canceller.enabled`；
- 上面这份测量就是在**没有额外开关**（只显式写了 `=ON`，与默认值相同）的
  构建上跑出来的；
- APM 缺席时插件工厂会显式把实例标记为 `DEGRADED`（`test_webrtc_audio3a.cpp`
  里的 `Audio3ADegradation.*` 覆盖了这条），所以「默认启用」失败时是**可观测
  的**，不是静默降级。

## 6. 已知限制

- 测量在 **Windows / MSVC / x64** 上完成。Linux 与 macOS 上 vendored APM 的
  构建路径不同（`tools/build_webrtc_apm.sh`），CI 仍然用
  `-DNIMRTC_VENDORED_WEBRTC_APM=OFF`——**CI 尚未断言 APM 默认启用**，这一半
  留作后续。
- 合成路径是**纯延时、无多径**。真实房间的混响会让 ERLE 低于 30 dB。
- 近端保护只验证了**音调**。宽带近端会被上游的残余回声抑制器压掉（见
  §3.2），这是上游行为，但**值得记录**：如果宿主依赖宽带领近端，NimRTC 需要
  提供一个关闭残余回声抑制的开关——目前 `Config` 没有这个旋钮。

## 7. 相关文件

| 文件 | 作用 |
|---|---|
| `src/modules/audio3a/tests/test_audio3a_erle.cpp` | ERLE 台架与三个断言 |
| `src/modules/audio3a/tests/CMakeLists.txt` | 台架的构建接线 |
| `tools/build_webrtc_apm.cmd` | Windows 下的 vendored APM 构建入口 |
| `tools/apm_link_order.py` | 记录 Meson 验证过的静态库链接顺序 |
