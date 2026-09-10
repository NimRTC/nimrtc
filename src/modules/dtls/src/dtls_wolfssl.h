/**
 * dtls_wolfssl.h - wolfSSL DTLS wrapper for NimRTC
 * 
 * 提供与原有 dtls.cpp 相同的 API 接口
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 错误码
#define DTLS_OK           0
#define DTLS_ERROR       -1
#define DTLS_WANT_READ   -2
#define DTLS_WANT_WRITE  -3
#define DTLS_TIMEOUT     -4

// 连接上下文 (opaque handle)
typedef void* DtlsContext;

// 初始化/清理
int dtls_wolfssl_init(void);
void dtls_wolfssl_cleanup(void);

// 创建/销毁
DtlsContext dtls_wolfssl_create_server(const char* cert_path, const char* key_path);
DtlsContext dtls_wolfssl_create_client(const char* ca_path);
void dtls_wolfssl_destroy(DtlsContext ctx);

// 绑定地址 (server)
int dtls_wolfssl_bind(DtlsContext ctx, const char* ip, int port);

// 连接 (client)
int dtls_wolfssl_connect(DtlsContext ctx, const char* ip, int port);

// 接受连接 (server)
DtlsContext dtls_wolfssl_accept(DtlsContext ctx);

// 握手
int dtls_wolfssl_handshake(DtlsContext ctx);

// 发送/接收
int dtls_wolfssl_send(DtlsContext ctx, const uint8_t* data, int len);
int dtls_wolfssl_recv(DtlsContext ctx, uint8_t* buf, int max_len);

// SRTP 密钥导出
int dtls_wolfssl_export_srtp(DtlsContext ctx,
                              uint8_t* client_key, int* client_key_len,
                              uint8_t* server_key, int* server_key_len);

// 证书指纹
const char* dtls_wolfssl_get_fingerprint(DtlsContext ctx);

// 关闭
void dtls_wolfssl_close(DtlsContext ctx);

// 获取协商的密码套件
const char* dtls_wolfssl_get_cipher(DtlsContext ctx);

#ifdef __cplusplus
}
#endif
