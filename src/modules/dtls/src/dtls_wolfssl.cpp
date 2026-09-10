/**
 * dtls_wolfssl.cpp - wolfSSL DTLS wrapper implementation
 */

#define _WIN32_WINNT 0x0601

#include "dtls_wolfssl.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/asn.h>

#include <cstdio>
#include <cstring>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// 全局 wolfSSL 状态
static std::atomic<bool> g_wolfssl_init(false);

// SRTP 长度
#define SRTP_MASTER_KEY_LEN 16
#define SRTP_MASTER_SALT_LEN 14

// 内部实现结构体
struct DtlsContextImpl {
    WOLFSSL*             ssl = nullptr;
    WOLFSSL_CTX*         ctx = nullptr;
    SOCKET               sock = INVALID_SOCKET;
    SOCKADDR_IN          peer_addr{};
    bool                 is_server = false;
    bool                 is_connected = false;
    char                 fingerprint[128] = {0};
    char                 cipher[64] = {0};
    uint8_t              srtp_keys[80] = {0};
    bool                 has_srtp_keys = false;
};

// 计算 PEM 证书文件的 SHA-256 指纹
static int compute_pem_fingerprint(const char* cert_path, char* fp_out, size_t fp_size) {
    if (!cert_path || !fp_out || fp_size < 96) return DTLS_ERROR;

    // 读取 PEM 文件
    FILE* f = nullptr;
    if (fopen_s(&f, cert_path, "rb") != 0 || !f) {
        return DTLS_ERROR;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 1 * 1024 * 1024) {
        fclose(f);
        return DTLS_ERROR;
    }

    unsigned char* pem = (unsigned char*)malloc((size_t)sz + 1);
    if (!pem) { fclose(f); return DTLS_ERROR; }

    size_t n = fread(pem, 1, (size_t)sz, f);
    fclose(f);
    pem[n] = '\0';

    // PEM → DER (CERT_TYPE = 证书)
    // wc_CertPemToDer: 需要预先分配 DER 缓冲区
    int max_der = (int)(n * 3 / 2 + 64);  // 粗略估计
    unsigned char* der = (unsigned char*)malloc((size_t)max_der);
    if (!der) {
        free(pem);
        return DTLS_ERROR;
    }

    int der_sz = wc_CertPemToDer(pem, (int)n, der, max_der, CERT_TYPE);
    free(pem);

    if (der_sz <= 0) {
        free(der);
        return DTLS_ERROR;
    }

    // DER SHA-256
    unsigned char hash[32];
    int rc = wc_Sha256Hash(der, der_sz, hash);
    free(der);

    if (rc != 0) return DTLS_ERROR;

    // 格式: sha256/HEX=
    size_t pos = 0;
    if (fp_size < 96) return DTLS_ERROR;
    strcpy_s(fp_out, fp_size, "sha256/");
    pos = 7;
    for (int i = 0; i < 32 && pos + 3 < fp_size; i++) {
        sprintf_s(fp_out + pos, fp_size - pos, "%02X", hash[i]);
        pos += 2;
    }
    fp_out[pos++] = '=';
    fp_out[pos] = '\0';

    return DTLS_OK;
}

// 初始化 wolfSSL
int dtls_wolfssl_init(void) {
    if (g_wolfssl_init.load()) {
        return DTLS_OK;
    }
    
    int ret = wolfSSL_Init();
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Init failed: %d\n", ret);
        return DTLS_ERROR;
    }
    
    g_wolfssl_init.store(true);
    printf("[wolfSSL] Initialized\n");
    return DTLS_OK;
}

// 清理
void dtls_wolfssl_cleanup(void) {
    if (g_wolfssl_init.load()) {
        wolfSSL_Cleanup();
        g_wolfssl_init.store(false);
        printf("[wolfSSL] Cleanup done\n");
    }
}

// 创建服务器上下文
DtlsContext dtls_wolfssl_create_server(const char* cert_path, const char* key_path) {
    DtlsContextImpl* ctx = new DtlsContextImpl();
    ctx->is_server = true;
    
    // 创建 DTLS 1.2 上下文
    ctx->ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    if (!ctx->ctx) {
        printf("[wolfSSL] Server: Failed to create CTX\n");
        delete ctx;
        return nullptr;
    }
    
    // 设置 WebRTC 兼容密码套件
    int ret = wolfSSL_CTX_set_cipher_list(ctx->ctx,
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-SHA256");
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Server: Failed to set cipher list\n");
        wolfSSL_CTX_free(ctx->ctx);
        delete ctx;
        return nullptr;
    }
    
    // 加载证书
    ret = wolfSSL_CTX_use_certificate_file(ctx->ctx, cert_path, WOLFSSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Server: Failed to load cert from %s\n", cert_path);
        wolfSSL_CTX_free(ctx->ctx);
        delete ctx;
        return nullptr;
    }
    
    // 加载私钥
    ret = wolfSSL_CTX_use_PrivateKey_file(ctx->ctx, key_path, WOLFSSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Server: Failed to load key from %s\n", key_path);
        wolfSSL_CTX_free(ctx->ctx);
        delete ctx;
        return nullptr;
    }
    
    // 验证私钥
    if (!wolfSSL_CTX_check_private_key(ctx->ctx)) {
        printf("[wolfSSL] Server: Private key does not match certificate\n");
        wolfSSL_CTX_free(ctx->ctx);
        delete ctx;
        return nullptr;
    }

    // 计算并保存证书指纹
    if (compute_pem_fingerprint(cert_path, ctx->fingerprint, sizeof(ctx->fingerprint)) == DTLS_OK) {
        printf("[wolfSSL] Server: Fingerprint = %s\n", ctx->fingerprint);
    } else {
        printf("[wolfSSL] Server: Warning - fingerprint computation failed\n");
    }

    wolfSSL_CTX_set_verify(ctx->ctx, WOLFSSL_VERIFY_NONE, NULL);

    // 设置 SRTP profile (RFC 5764)
    ret = wolfSSL_CTX_set_tlsext_use_srtp(ctx->ctx, "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32");
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Server: Warning - SRTP profile not set (%d)\n", ret);
    }

    printf("[wolfSSL] Server context created\n");
    return (DtlsContext)ctx;
}

// 创建客户端上下文
DtlsContext dtls_wolfssl_create_client(const char* ca_path) {
    DtlsContextImpl* ctx = new DtlsContextImpl();
    ctx->is_server = false;
    
    ctx->ctx = wolfSSL_CTX_new(wolfDTLSv1_2_client_method());
    if (!ctx->ctx) {
        printf("[wolfSSL] Client: Failed to create CTX\n");
        delete ctx;
        return nullptr;
    }
    
    // 设置 WebRTC 兼容密码套件
    int ret = wolfSSL_CTX_set_cipher_list(ctx->ctx,
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-SHA256");
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Client: Failed to set cipher list\n");
        wolfSSL_CTX_free(ctx->ctx);
        delete ctx;
        return nullptr;
    }
    
    if (ca_path && ca_path[0]) {
        ret = wolfSSL_CTX_load_verify_locations(ctx->ctx, ca_path, NULL);
        if (ret != WOLFSSL_SUCCESS) {
            printf("[wolfSSL] Client: Warning - failed to load CA from %s\n", ca_path);
        }
    }

    // 设置 SRTP profile (RFC 5764)
    ret = wolfSSL_CTX_set_tlsext_use_srtp(ctx->ctx, "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32");
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] Client: Warning - SRTP profile not set (%d)\n", ret);
    }

    printf("[wolfSSL] Client context created\n");
    return (DtlsContext)ctx;
}

// 销毁上下文
void dtls_wolfssl_destroy(DtlsContext handle) {
    if (!handle) return;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    
    dtls_wolfssl_close(handle);
    
    if (ctx->ctx) {
        wolfSSL_CTX_free(ctx->ctx);
        ctx->ctx = nullptr;
    }
    
    delete ctx;
}

// 绑定地址 (server)
int dtls_wolfssl_bind(DtlsContext handle, const char* ip, int port) {
    if (!handle) return DTLS_ERROR;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    
    ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->sock == INVALID_SOCKET) {
        printf("[wolfSSL] socket() failed: %d\n", WSAGetLastError());
        return DTLS_ERROR;
    }
    
    SOCKADDR_IN addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (ip && ip[0]) {
        inet_pton(AF_INET, ip, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    
    if (bind(ctx->sock, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[wolfSSL] bind() failed: %d\n", WSAGetLastError());
        closesocket(ctx->sock);
        ctx->sock = INVALID_SOCKET;
        return DTLS_ERROR;
    }
    
    printf("[wolfSSL] Bound to %s:%d\n", ip ? ip : "0.0.0.0", port);
    return DTLS_OK;
}

// 连接 (client)
int dtls_wolfssl_connect(DtlsContext handle, const char* ip, int port) {
    if (!handle) return DTLS_ERROR;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    
    if (ctx->sock == INVALID_SOCKET) {
        ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ctx->sock == INVALID_SOCKET) {
            return DTLS_ERROR;
        }
    }
    
    memset(&ctx->peer_addr, 0, sizeof(ctx->peer_addr));
    ctx->peer_addr.sin_family = AF_INET;
    ctx->peer_addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &ctx->peer_addr.sin_addr);
    
    ctx->is_connected = true;
    printf("[wolfSSL] Connecting to %s:%d\n", ip, port);
    return DTLS_OK;
}

// 接受连接 (服务器)
DtlsContext dtls_wolfssl_accept(DtlsContext handle) {
    if (!handle) return nullptr;
    DtlsContextImpl* listener_ctx = (DtlsContextImpl*)handle;
    if (!listener_ctx->ctx) return nullptr;
    
    DtlsContextImpl* client_ctx = new DtlsContextImpl();
    client_ctx->is_server = true;
    client_ctx->ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    if (!client_ctx->ctx) {
        delete client_ctx;
        return nullptr;
    }
    
    wolfSSL_CTX_set_cipher_list(client_ctx->ctx,
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-SHA256");
    
    wolfSSL_CTX_use_certificate_file(client_ctx->ctx, "certs/server-cert.pem", WOLFSSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(client_ctx->ctx, "certs/server-key.pem", WOLFSSL_FILETYPE_PEM);
    wolfSSL_CTX_set_verify(client_ctx->ctx, WOLFSSL_VERIFY_NONE, NULL);
    
    client_ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client_ctx->sock == INVALID_SOCKET) {
        wolfSSL_CTX_free(client_ctx->ctx);
        delete client_ctx;
        return nullptr;
    }
    
    client_ctx->is_connected = true;
    
    return (DtlsContext)client_ctx;
}

// 握手
int dtls_wolfssl_handshake(DtlsContext handle) {
    if (!handle) return DTLS_ERROR;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    if (!ctx->ctx) return DTLS_ERROR;
    
    if (ctx->ssl) {
        wolfSSL_free(ctx->ssl);
    }
    ctx->ssl = wolfSSL_new(ctx->ctx);
    if (!ctx->ssl) {
        printf("[wolfSSL] Handshake: Failed to create SSL\n");
        return DTLS_ERROR;
    }
    
    // wolfSSL_set_fd 接受 int, 但 SOCKET 是 unsigned
    // 使用 wolfSSL_SetIOReadCtx/WriteCtx 自定义回调更好
    // 这里简单 cast
    wolfSSL_set_fd(ctx->ssl, (int)(intptr_t)ctx->sock);
    
    wolfSSL_dtls_set_peer(ctx->ssl, &ctx->peer_addr, sizeof(ctx->peer_addr));
    
    int ret;
    if (ctx->is_server) {
        ret = wolfSSL_accept(ctx->ssl);
    } else {
        ret = wolfSSL_connect(ctx->ssl);
    }
    
    if (ret != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(ctx->ssl, ret);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            return DTLS_WANT_READ;
        }
        printf("[wolfSSL] Handshake failed: %d (%s)\n", err, wolfSSL_ERR_reason_error_string(err));
        return DTLS_ERROR;
    }
    
    const char* cipher = wolfSSL_get_cipher(ctx->ssl);
    if (cipher) {
        strncpy_s(ctx->cipher, sizeof(ctx->cipher), cipher, _TRUNCATE);
    }
    
    ctx->has_srtp_keys = false;
    
    printf("[wolfSSL] Handshake complete! Cipher: %s\n", ctx->cipher);
    return DTLS_OK;
}

// 发送数据
int dtls_wolfssl_send(DtlsContext handle, const uint8_t* data, int len) {
    if (!handle) return DTLS_ERROR;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    if (!ctx->ssl) return DTLS_ERROR;
    
    int ret = wolfSSL_write(ctx->ssl, data, len);
    if (ret < 0) {
        int err = wolfSSL_get_error(ctx->ssl, ret);
        printf("[wolfSSL] Send error: %d\n", err);
        return DTLS_ERROR;
    }
    
    return ret;
}

// 接收数据
int dtls_wolfssl_recv(DtlsContext handle, uint8_t* buf, int max_len) {
    if (!handle) return DTLS_ERROR;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    if (!ctx->ssl) return DTLS_ERROR;
    
    int ret = wolfSSL_read(ctx->ssl, buf, max_len);
    if (ret < 0) {
        int err = wolfSSL_get_error(ctx->ssl, ret);
        if (err == WOLFSSL_ERROR_WANT_READ) {
            return DTLS_WANT_READ;
        }
        if (err == WOLFSSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        printf("[wolfSSL] Recv error: %d\n", err);
        return DTLS_ERROR;
    }
    
    return ret;
}

// SRTP 密钥导出 (RFC 5764)
int dtls_wolfssl_export_srtp(DtlsContext handle,
                               uint8_t* client_key, int* client_key_len,
                               uint8_t* server_key, int* server_key_len) {
    if (!handle) return DTLS_ERROR;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    if (!ctx->ssl) return DTLS_ERROR;
    
    if (wolfSSL_is_init_finished(ctx->ssl) != 1) {
        printf("[wolfSSL] SRTP export: Handshake not complete\n");
        return DTLS_ERROR;
    }

    // 获取 SRTP profile
    const WOLFSSL_SRTP_PROTECTION_PROFILE* srtp_profile =
        wolfSSL_get_selected_srtp_profile(ctx->ssl);
    if (!srtp_profile) {
        printf("[wolfSSL] SRTP export: No SRTP profile negotiated\n");
        return DTLS_ERROR;
    }
    printf("[wolfSSL] SRTP profile: %s (id=%d)\n",
           srtp_profile->name, srtp_profile->id);

    // RFC 5764 规定的 keying material 格式:
    // client_master_key (16) + server_master_key (16)
    // + client_master_salt (14) + server_master_salt (14) = 60 bytes
    unsigned char keying_material[80];  // 留一些余量
    size_t olen = sizeof(keying_material);

    int ret = wolfSSL_export_dtls_srtp_keying_material(ctx->ssl,
                                                       keying_material, &olen);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[wolfSSL] SRTP export: keying material export failed (%d)\n", ret);
        return DTLS_ERROR;
    }

    if (olen < 60) {
        printf("[wolfSSL] SRTP export: keying material too short (%zu bytes)\n", olen);
        return DTLS_ERROR;
    }

    // 解析 keying material
    // client_write_key = client_master_key (16) + client_master_salt (14) = 30 bytes
    // server_write_key = server_master_key (16) + server_master_salt (14) = 30 bytes
    if (client_key && client_key_len) {
        memcpy(client_key, keying_material + 16, 14);  // client_master_salt
        memcpy(client_key + 14, keying_material, 16); // client_master_key
        *client_key_len = 30;
    }

    if (server_key && server_key_len) {
        memcpy(server_key, keying_material + 46, 14);  // server_master_salt
        memcpy(server_key + 14, keying_material + 32, 16); // server_master_key
        *server_key_len = 30;
    }

    ctx->has_srtp_keys = true;

    // 打印密钥 (十六进制, 便于调试)
    printf("[wolfSSL] SRTP keys exported (%zu bytes):\n", olen);
    printf("  client_key: ");
    for (int i = 0; i < 30; i++) printf("%02X", client_key ? client_key[i] : 0);
    printf("\n");
    printf("  server_key: ");
    for (int i = 0; i < 30; i++) printf("%02X", server_key ? server_key[i] : 0);
    printf("\n");

    return DTLS_OK;
}

// 获取指纹
const char* dtls_wolfssl_get_fingerprint(DtlsContext handle) {
    if (!handle) return "";
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    return ctx->fingerprint;
}

// 关闭
void dtls_wolfssl_close(DtlsContext handle) {
    if (!handle) return;
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    
    if (ctx->ssl) {
        wolfSSL_shutdown(ctx->ssl);
        wolfSSL_free(ctx->ssl);
        ctx->ssl = nullptr;
    }
    
    if (ctx->sock != INVALID_SOCKET) {
        closesocket(ctx->sock);
        ctx->sock = INVALID_SOCKET;
    }
}

// 获取密码套件
const char* dtls_wolfssl_get_cipher(DtlsContext handle) {
    if (!handle) return "";
    DtlsContextImpl* ctx = (DtlsContextImpl*)handle;
    return ctx->cipher;
}
