// 验收测试 —— 切片 v1-game-source（ADR-034）
//
// 每条验收标准对应一个真实断言（`test`），骨架里的占位符已全部替换为真实断言。
// 断言方式与规格「验证方式」一致：对切片改动（examples/remote-desktop/ 下
// 的两个源文件）做源码级检查 —— 读源配置默认值、确认开关逻辑、确认帧序号
// 单调递增的写入路径、确认本片改动不碰控面传输与数据通道。
//
// 这些断言是确定性的（不依赖网络、显示器、编码器可用性），门禁可以直接跑。
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

// ---------------------------------------------------------------------------
// 定位切片源文件（本文件位于 <repo>/tests/acceptance/v1-game-source/）
// ---------------------------------------------------------------------------

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');

/** 读取一个 repo 相对路径的文本文件（以 \n 归一化换行）。 */
function readSource(relPath) {
  const abs = path.join(REPO_ROOT, relPath);
  assert.ok(fs.existsSync(abs), `源文件不存在: ${relPath}`);
  return fs.readFileSync(abs, 'utf8').replace(/\r\n/g, '\n');
}

const rdDevice = readSource('examples/remote-desktop/rd_device.cpp');
const gameSource = readSource('examples/remote-desktop/game_source.hpp');

/** 取两个锚点之间的文本（含锚点），找不到即抛错。 */
function sliceBetween(text, startAnchor, endAnchor) {
  const i = text.indexOf(startAnchor);
  const j = text.indexOf(endAnchor, i + startAnchor.length);
  assert.ok(i !== -1, `找不到锚点: ${startAnchor}`);
  assert.ok(j !== -1, `找不到锚点: ${endAnchor}`);
  assert.ok(j > i, `锚点顺序错误: ${startAnchor} -> ${endAnchor}`);
  return text.slice(i, j + endAnchor.length);
}

// ---------------------------------------------------------------------------
// 验收标准 1：合成打地鼠画面以帧计数叠加模式按 60fps 灌入，帧序号单调递增
// ---------------------------------------------------------------------------

test(
  '合成打地鼠画面以帧计数叠加模式按 60fps 灌入，帧序号单调递增',
  () => {
    // ---- 打地鼠画面存在：3x3 洞 + 地鼠 + 帧计数叠加 ----------------------
    assert.match(gameSource, /kHoleColumns\s*=\s*3/);
    assert.match(gameSource, /kHoleRows\s*=\s*3/);
    // render_frame 里每一帧都画帧计数叠加（顶左角），这就是「帧计数叠加模式」。
    assert.match(
      gameSource,
      /draw_frame_counter\s*\(\s*dst\s*,\s*w\s*,\s*h\s*,\s*8u\s*,\s*8u\s*,\s*frame_index\s*\)/,
    );
    // 地鼠：头部 + 两只眼睛（render_frame 内部）。
    assert.match(gameSource, /const std::int32_t head_radius/);
    assert.match(gameSource, /fill_circle\s*\(dst, w, h, hole\.cx, head_cy, head_radius/);

    // ---- 固定步进 60fps：producer_loop 以 1/fps 的周期睡到帧截止点 --------
    const loop = sliceBetween(
      gameSource,
      'void GameSource::producer_loop() noexcept {',
      'void GameSource::draw_digit(',
    );
    assert.match(loop, /1'000'000\s*\/\s*static_cast<std::int64_t>\(fps\)/);
    assert.match(loop, /std::this_thread::sleep_until\(next\)/);
    // 默认帧率 60（open() 在 fps==0 时补 60）。
    assert.match(gameSource, /cfg_\.format\.fps\s*==\s*0/);

    // ---- 帧序号单调递增：入队帧的 seq 就是 frame_index_，且 push 后自增 --
    const produce = sliceBetween(
      gameSource,
      'plugins::Status GameSource::produce_frame() noexcept {',
      'void GameSource::producer_loop() noexcept {',
    );
    // queue_.push(..., w, h, ts, frame_index_, dirty) —— 第 8 个参数是 seq。
    assert.match(
      produce,
      /queue_\.push\s*\(\s*scratch_\.data\(\)\s*,\s*scratch_\.size\(\)\s*,\s*static_cast<std::int32_t>\(w \* 4u\)\s*,\s*w\s*,\s*h\s*,\s*ts\s*,\s*frame_index_\s*,\s*dirty\s*\)/,
    );
    // push 之后才 ++frame_index_：保证每一帧的 seq 严格递增（0,1,2,...）。
    const pushPos = produce.indexOf('queue_.push');
    const incPos = produce.indexOf('++frame_index_;');
    assert.ok(pushPos !== -1 && incPos !== -1);
    assert.ok(
      incPos > pushPos,
      'frame_index_ 必须在 push 之后自增，才能保证入队帧序号严格递增',
    );
  },
);

// ---------------------------------------------------------------------------
// 验收标准 2：设备端新增游戏源开关，缺省走原有真实屏幕采集链路，只有显式
//             传参才走合成游戏源
// ---------------------------------------------------------------------------

test(
  '设备端新增游戏源开关，缺省走原有真实屏幕采集链路，只有显式传参才走合成游戏源',
  () => {
    // ---- 缺省关闭：Options::game_source 默认 false -------------------------
    assert.match(rdDevice, /bool\s+game_source\s*=\s*false\s*;/);

    // ---- 只有 --game-source 传参才打开：整份文件里只有一处赋 true，且在
    //      main() 的参数解析里 ----------------------------------------------
    const assignments = rdDevice.match(/game_source\s*=\s*true\s*;/g) || [];
    assert.strictEqual(
      assignments.length,
      1,
      'game_source 只能有一处赋 true（--game-source 参数解析），不得有隐式默认开启',
    );
    assert.ok(rdDevice.includes('else if (arg == "--game-source")'));

    // ---- 采集配置二选一：缺省分支走真实屏幕，传参分支走合成游戏源 ---------
    const captureConfig = sliceBetween(
      rdDevice,
      'if (opts_.game_source) {',
      '// The device wants the pointer composited',
    );

    // 传参分支：注册 game-source 工厂并把引擎采集名设成它。
    // 工厂注册调用在源码里跨两行（100 列限制下的 clang-format 换行），
    // 所以这里用容忍空白（含换行）的正则断言，而不是字面量 includes。
    assert.ok(captureConfig.includes('static nimrtc::rd::GameSourceFactory game_factory;'));
    assert.match(
      captureConfig,
      /register_video_capture\s*\(\s*game_factory\.id\(\),\s*&game_factory\)/,
    );
    assert.ok(captureConfig.includes('cfg.video_capture_name = std::string(game_factory.id());'));
    // 工厂 id 就是 "game-source"（game_source.hpp）。
    assert.match(gameSource, /return\s+"game-source"\s*;/);

    // 缺省分支：名字来自平台默认采集后端（真实屏幕），与旧行为一致。
    assert.ok(captureConfig.includes('? default_capture_name()'));
    assert.ok(captureConfig.includes('cfg.video_capture_name = opts_.capture_name.empty()'));
    // 缺省分支保留原有帧率行为（30fps 屏幕采集，合成源不受影响）。
    assert.match(captureConfig, /cfg\.video_fps\s*=\s*30;/);
  },
);

// ---------------------------------------------------------------------------
// 验收标准 3：合成源分辨率默认 720p 地板保可读，帧率可配
// ---------------------------------------------------------------------------

test(
  '合成源分辨率默认 720p 地板保可读，帧率可配',
  () => {
    // ---- 游戏源 open() 的 720p 地板 + 60fps 默认值 ------------------------
    const open = sliceBetween(
      gameSource,
      'plugins::Status GameSource::open() noexcept {',
      'void GameSource::close() noexcept {',
    );
    assert.match(open, /cfg_\.format\.width\s*==\s*0\s*\)\s*cfg_\.format\.width\s*=\s*1280;/);
    assert.match(open, /cfg_\.format\.height\s*==\s*0\s*\)\s*cfg_\.format\.height\s*=\s*720;/);
    assert.match(open, /cfg_\.format\.fps\s*==\s*0\s*\)\s*cfg_\.format\.fps\s*=\s*60;/);

    // ---- supported_formats 也广告 1280x720 ---------------------------------
    assert.match(gameSource, /\{1280,\s*720\}/);

    // ---- rd_device：--game-fps 可配，宽高可配，缺省 1280x720@60 ------------
    assert.match(rdDevice, /std::uint32_t\s+game_fps\s*=\s*60\s*;/);
    assert.ok(rdDevice.includes('else if (arg == "--game-fps" && has_next)'));
    const gameBranch = sliceBetween(
      rdDevice,
      'if (opts_.game_source) {',
      '} else {',
    );
    // 缺省宽高落到 1280x720（720p 地板），帧率用 --game-fps。
    assert.match(gameBranch, /opts_\.width\s*!=\s*0\s*\?\s*opts_\.width\s*:\s*1280/);
    assert.match(gameBranch, /opts_\.height\s*!=\s*0\s*\?\s*opts_\.height\s*:\s*720/);
    assert.match(gameBranch, /cfg\.video_capture_config\.format\.fps\s*=\s*opts_\.game_fps/);
  },
);

// ---------------------------------------------------------------------------
// 验收标准 4：本片不碰控面传输配置，不碰数据通道控面
// ---------------------------------------------------------------------------

test(
  '本片不碰控面传输配置，不碰数据通道控面',
  () => {
    // ---- 游戏源头文件是纯采集缝：不得引用控面 / 数据通道 / 传输配置 -------
    // 注意 `ice_` 用词边界正则（\bice_）：`device_id` 是采集缝的接口字段
    // （plugins::CaptureConfig::device_id），必然出现在任何采集设备里，
    // 不能当成传输配置符号；而真正的传输引用（ice_transport 等）前面有
    // 非单词字符，仍会被 \bice_ 命中。
    for (const needle of [
      'control_channel',
      'control-plane',
      'data_channel',
      'datachannel',
      'stun_server',
      'stun_host',
      'turn_host',
      'turn_user',
      /\bice_/,
      'dtls',
    ]) {
      if (needle instanceof RegExp) {
        assert.doesNotMatch(
          gameSource,
          needle,
          `game_source.hpp 不得出现控面/数据通道/传输配置符号: ${needle}`,
        );
      } else {
        assert.ok(
          !gameSource.includes(needle),
          `game_source.hpp 不得出现控面/数据通道/传输配置符号: ${needle}`,
        );
      }
    }

    // ---- rd_device 的采集配置块只碰采集与视频配置，不碰控面/数据通道 ------
    const captureConfig = sliceBetween(
      rdDevice,
      'if (opts_.game_source) {',
      '// The device wants the pointer composited',
    );
    for (const needle of [
      'control_channel_name',
      'data_channel',
      'datachannel',
      'stun_server',
      'stun_host',
      'stun_port',
      'turn_host',
      'turn_port',
      'turn_user',
      'turn_pass',
    ]) {
      assert.ok(
        !captureConfig.includes(needle),
        `采集配置块不得设置控面/数据通道/传输配置字段: ${needle}`,
      );
    }
    // 采集配置块里出现的 cfg 字段只有采集与视频路径的（白名单确认改动边界）。
    const cfgAssignments = captureConfig.match(/cfg\.[A-Za-z_][A-Za-z0-9_.]*/g) || [];
    assert.ok(cfgAssignments.length > 0, '采集配置块应包含 cfg 赋值');
    for (const field of cfgAssignments) {
      assert.ok(
        /^cfg\.(video_capture_|video_|video_capture_config\.)/.test(field),
        `采集配置块设置了超出本片范围的字段: ${field}`,
      );
    }

    // ---- Options 与参数解析的新增项只含游戏源开关/帧率两项 -----------------
    const optionsAdditions = ['bool game_source = false;', 'std::uint32_t game_fps = 60;'];
    for (const line of optionsAdditions) {
      assert.ok(rdDevice.includes(line), `Options 缺少新增项: ${line}`);
    }
    for (const flag of ['--game-source', '--game-fps']) {
      assert.ok(rdDevice.includes(flag), `参数解析缺少开关: ${flag}`);
    }
  },
);
