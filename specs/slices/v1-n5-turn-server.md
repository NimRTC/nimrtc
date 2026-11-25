# Slice: v1-n5-turn-server

## 目的
本切片为 `examples/turn-server/` 提供参考 TURN 部署形态：默认在 Linux CI leg 上原生安装 coturn，生成自签证书并以 TLS-only 模式启动；compose 文件仅保留为开发者路径与归档证据路径。验收测试只依赖一个 TURN URI 和一份凭据，不绑定服务器的启动方式。

## 范围
- examples/turn-server/**
- tests/acceptance/v1-n5-turn-server/acceptance.test.cjs

## 需求
- N5：CI 默认形态为 Linux leg 上原生安装 coturn（apt-get install coturn + 生成自签证书 + --tls-only），compose 文件留给开发者路径与归档证据路径；测试只认“一个 TURN URI 和一份凭据”，不关心服务器如何启动。

## 验收标准
1. `examples/turn-server/` 下提供 Linux 原生安装 coturn 的参考脚本或配置，其中包含 `apt-get install coturn`；验证方式：查看该路径下文件内容，可检索到 `apt-get install coturn` 命令。
2. 参考部署包含生成自签名证书的步骤，证书用于 TURN 的 TLS 监听；验证方式：查看 `examples/turn-server/` 下脚本或文档，存在 `openssl req` 或等效自签证书生成命令，且启动配置引用该证书路径。
3. 参考部署启动 coturn 时启用 TLS-only 模式，命令或配置包含 `--tls-only`；验证方式：静态检查 `examples/turn-server/` 下的启动脚本或配置文件，存在 `--tls-only` 参数。
4. 若 `examples/turn-server/` 中包含 compose 文件，该文件仅作为开发者路径或归档证据，不作为验收测试启动 TURN 的前置条件；验证方式：验收测试文件不包含 `docker compose`、`docker-compose` 或相关容器生命周期命令。
5. `tests/acceptance/v1-n5-turn-server/acceptance.test.cjs` 只读取一个 TURN URI 和一份凭据，不自行安装、启动或停止 coturn；验证方式：检查测试源码，不包含 `apt-get`、`systemctl`、`service`、`docker` 等服务器生命周期管理命令，TURN 接入参数集中来自 URI 与凭据变量。
6. 测试通过给定的 TLS TURN URI 与凭据实际发起交互并验证 TURN 服务可用；验证方式：提供可用的 TLS TURN URI 和凭据后运行 `node --test tests/acceptance/v1-n5-turn-server/acceptance.test.cjs`，测试中发送 TURN 分配或绑定请求，并断言收到成功响应或有效中继地址。

## 风险
- coturn 版本差异可能导致 `--tls-only` 与 `--require-tls` 参数名不一致；应对：实现以需求正文的 `--tls-only` 为准，并在脚本注释或文档中标注若使用 `--require-tls` 需核对 coturn 版本。
- 自签证书可能不被 TURN 客户端信任，导致连接失败；应对：测试客户端显式允许未受信任证书，或在参考部署说明中明确客户端跳过证书校验的方式。
- `apt-get install coturn` 需要 root 权限且可能交互；应对：脚本使用 `DEBIAN_FRONTEND=noninteractive`，并确保 Linux CI leg 具备所需权限。
- 测试环境未提供 TURN URI 或凭据时会产生误报失败；应对：测试在缺失接入参数时给出明确报错，CI 默认 Linux leg 预先提供这些变量。
- 验收测试误引入服务器启动逻辑，会违背“不关心服务器如何启动”的需求；应对：实现和评审重点检查 `acceptance.test.cjs` 只读 URI 与凭据，不包含 coturn 或 compose 生命周期命令。
