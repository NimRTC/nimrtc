# NimRTC wolfSSL 集成方案

## 问题根源

NimRTC 当前 dtls.cpp 实现与 Chrome/BoringSSL 握手失败，原因是：
- 证书是 RSA 密钥，但代码配置了 ECDSA 密码套件
- `Can't match cipher suite` 错误

**验证结果**：wolfSSL DTLS 已成功实现相同握手流程。

---

## 集成方案

### 目录结构

```
src/modules/dtls/
├── src/
│   ├── dtls.cpp              # 原有代码 (保留)
│   ├── dtls_wolfssl.cpp      # 新增: wolfSSL 封装层
│   └── dtls_wolfssl.h        # 新增: wolfSSL 头文件
└── CMakeLists.txt            # 修改: 添加 wolfSSL
```

### 步骤 1: 创建 wolfSSL 封装层

**dtls_wolfssl.h**
```cpp
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 wolfSSL
int dtls_wolfssl_init(void);

// 创建 DTLS 连接
void* dtls_wolfssl_create(bool is_server, const char* cert_path, const char* key_path);

// 握手
int dtls_wolfssl_handshake(void* ctx);

// 发送数据
int dtls_wolfssl_send(void* ctx, const uint8_t* data, int len);

// 接收数据
int dtls_wolfssl_recv(void* ctx, uint8_t* buf, int max_len);

// 获取 SRTP 密钥 (握手后)
int dtls_wolfssl_get_srtp_keys(void* ctx, uint8_t* client_key, uint8_t* server_key,
                               int* key_len);

// 关闭
void dtls_wolfssl_close(void* ctx);

// 获取指纹
const char* dtls_wolfssl_get_fingerprint(void* ctx);

// 清理
void dtls_wolfssl_cleanup(void);

#ifdef __cplusplus
}
#endif
```

### 步骤 2: 修改 NimRTC 绑定层

**src/nimrtc_dtls.nim** (新增条件编译)
```nim
## dtls_backend.nim
## 支持切换 dtls.cpp 或 wolfSSL

when defined(use_wolfssl):
  include dtls_wolfssl_c
  proc dtlsInit*(): int {.importc: "dtls_wolfssl_init".}
  proc dtlsCreate*(isServer: bool, cert, key: cstring): pointer {.importc: "dtls_wolfssl_create".}
  # ... 其他导入
else:
  # 原有 dtls.cpp 绑定
  proc dtlsInit*(): int {.importc: "dtls_init".}
  # ...
```

### 步骤 3: CMakeLists.txt 配置

```cmake
# 条件编译选项
option(USE_WOLFSSL "Use wolfSSL instead of custom DTLS" OFF)

if(USE_WOLFSSL)
    message(STATUS "Using wolfSSL for DTLS")
    
    # wolfSSL 已编译在 build/wolfssl/build/
    set(WOLFSSL_ROOT "${CMAKE_SOURCE_DIR}/build/wolfssl")
    set(WOLFSSL_INCLUDE "${WOLFSSL_ROOT}/build" CACHE PATH "wolfSSL include dir")
    set(WOLFSSL_LIB "${WOLFSSL_ROOT}/build/wolfssl.lib" CACHE PATH "wolfSSL library")
    
    include_directories(${WOLFSSL_INCLUDE})
    link_directories($<$<CONFIG:Debug>:${WOLFSSL_ROOT}/build>)
    
    set(DTLS_SOURCES 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dtls_wolfssl.cpp
    )
    
    set(EXTRA_LIBS 
        ${WOLFSSL_LIB}
        ws2_32.lib
        crypt32.lib
    )
    
    add_definitions(-DUSE_WOLFSSL)
else()
    message(STATUS "Using custom DTLS (dtls.cpp)")
    
    set(DTLS_SOURCES 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dtls.cpp
    )
endif()
```

### 步骤 4: 编译方式

```bash
# 原方式 (dtls.cpp)
cmake -B build .

# 新方式 (wolfSSL)
cmake -B build -DUSE_WOLFSSL=ON .
cmake --build build
```

---

## 保留代码清单

| 文件 | 操作 | 原因 |
|------|------|------|
| `dtls.cpp` | **保留** | 备用，回滚用 |
| `dtls.h` | **保留** | API 定义 |
| `nimrtc_dtls.nim` | **保留** | Nim 绑定 |
| `dtls_wolfssl.cpp` | **新增** | wolfSSL 封装 |
| `dtls_wolfssl.h` | **新增** | wolfSSL 头文件 |

---

## 测试验证

### 编译后测试

```bash
# 1. 启动服务器
./dtls_server.exe

# 2. 确认输出
# Fingerprint: sha256/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX=

# 3. 用 Chrome 访问 DTLS 测试页面验证
```

### 对比测试

| 测试项 | dtls.cpp | wolfSSL |
|--------|----------|---------|
| 自签名证书 | ✅ | ✅ |
| ECDHE-RSA 握手 | ❌ | ✅ |
| Chrome 互通 | ❌ | ✅ |
| SRTP 密钥导出 | ? | ✅ |

---

## 风险评估

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| wolfSSL 体积 | 二进制增加 ~1.7MB | 可裁剪不需要的算法 |
| 性能 | 略有下降 | wolfSSL 优化良好 |
| 依赖增加 | 一个新库 | 静态链接，无运行时依赖 |
| 证书格式 | 需要 PEM | 现有证书可用 openssl 转换 |

---

## 下一步

1. ✅ 已完成：wolfSSL 编译成功
2. ✅ 已完成：DTLS 握手测试通过
3. ⬜ 创建 `dtls_wolfssl.cpp` 封装层
4. ⬜ 修改 CMakeLists.txt
5. ⬜ 集成到 NimRTC Nim 代码
6. ⬜ 端到端测试

---

## 参考

- wolfSSL 文档: https://www.wolfssl.com/docs/
- wolfSSL DTLS 示例: `tests/wolfssl_dtls/dtls_server.cpp`
- WebRTC DTLS 要求: RFC 5764
