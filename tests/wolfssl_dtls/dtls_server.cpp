/**
 * @file dtls_server.cpp
 * @brief wolfSSL DTLS server for Chrome interop testing (Windows)
 *
 * Adapted from wolfSSL official server-dtls.c for Windows Winsock.
 *
 * Build:
 *   cmd /c 03_build_dtls_server.bat
 */

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/asn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define close_socket close
#endif

#define SERV_PORT   9999
#define MSGLEN      4096

// -----------------------------------------------------------------------------
// Generate fingerprint for Chrome --ignore-certificate-errors-spki-list
// -----------------------------------------------------------------------------

static int generate_certificate_fingerprint(const char* cert_file, char* out_b64, size_t b64_len) {
    int i;

    printf("[FP] Loading certificate: %s\n", cert_file);

    // Read the PEM file directly (DER-encoded certificate)
    FILE* fp = fopen(cert_file, "rb");
    if (!fp) {
        printf("    [ERROR] Cannot open cert file\n");
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char* pem_buf = (unsigned char*)malloc(size);
    fread(pem_buf, 1, size, fp);
    fclose(fp);

    // For PEM, we need to find the DER content between BEGIN/END markers
    // Look for "-----BEGIN CERTIFICATE-----"
    const char* begin_marker = "-----BEGIN CERTIFICATE-----";
    unsigned char* der_buf = NULL;
    int der_len = 0;

    char* begin_pos = strstr((char*)pem_buf, begin_marker);
    if (begin_pos) {
        // Skip to the next line
        begin_pos = strchr(begin_pos, '\n');
        if (begin_pos) begin_pos++;

        // Find end marker
        char* end_pos = strstr(begin_pos, "-----END CERTIFICATE-----");
        if (end_pos) {
            // Strip whitespace
            unsigned char* der_start = (unsigned char*)begin_pos;
            int pem_len = (int)(end_pos - begin_pos);
            der_buf = (unsigned char*)malloc(pem_len);
            int out = 0;
            for (int i = 0; i < pem_len; i++) {
                char c = begin_pos[i];
                if (c != '\r' && c != '\n' && c != ' ') {
                    der_buf[out++] = (unsigned char)c;
                }
            }
            der_len = out;
            der_buf[out] = 0;
        }
    } else {
        // No PEM header, assume raw DER
        der_buf = pem_buf;
        der_len = (int)size;
        pem_buf = NULL;
    }

    if (der_len == 0) {
        printf("    [ERROR] No DER content found\n");
        free(pem_buf);
        free(der_buf);
        return -1;
    }

    // Base64 decode
    static const char* b64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    int b64_lookup[256];
    memset(b64_lookup, -1, sizeof(b64_lookup));
    for (i = 0; i < 64; i++) b64_lookup[(unsigned char)b64_chars[i]] = i;

    unsigned char* der_decoded = (unsigned char*)malloc(der_len);
    int out_len = 0;
    int bits = 0;
    unsigned int buf = 0;

    for (i = 0; i < der_len; i++) {
        unsigned char c = der_buf[i];
        if (c == '=') break;
        int v = b64_lookup[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            der_decoded[out_len++] = (buf >> bits) & 0xFF;
        }
    }

    printf("    [OK] Decoded %d bytes DER\n", out_len);

    // SHA-256
    unsigned char hash[32];
    Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, der_decoded, out_len);
    wc_Sha256Final(&sha, hash);

    // Base64 encode the hash
    int out_pos = 0;
    for (i = 0; i < 32; i += 3) {
        unsigned b0 = hash[i];
        unsigned b1 = (i + 1 < 32) ? hash[i+1] : 0;
        unsigned b2 = (i + 2 < 32) ? hash[i+2] : 0;

        if (out_pos + 4 >= (int)b64_len) break;
        out_b64[out_pos++] = b64_chars[(b0 >> 2) & 0x3F];
        out_b64[out_pos++] = b64_chars[((b0 << 4) | (b1 >> 4)) & 0x3F];
        out_b64[out_pos++] = (i + 1 < 32) ? b64_chars[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=';
        out_b64[out_pos++] = (i + 2 < 32) ? b64_chars[b2 & 0x3F] : '=';
    }
    out_b64[out_pos] = 0;

    free(pem_buf);
    free(der_buf);
    free(der_decoded);

    return 0;
}

// -----------------------------------------------------------------------------
// Main DTLS server
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
    int ret;
    char fingerprint[64] = {0};

    const char* cert_file = "certs/server-cert.pem";
    const char* key_file = "certs/server-key.pem";

    if (argc >= 2) cert_file = argv[1];
    if (argc >= 3) key_file = argv[2];

    setbuf(stdout, NULL);  // Unbuffered output
    setbuf(stderr, NULL);

    printf("\n");
    printf("================================================================\n");
    printf("  wolfSSL DTLS Server (Chrome Interop Test)\n");
    printf("================================================================\n");
    printf("\n");

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#endif

    // Print fingerprint
    if (generate_certificate_fingerprint(cert_file, fingerprint, sizeof(fingerprint)) == 0) {
        printf("\n");
        printf("  Certificate SHA-256 fingerprint (base64):\n");
        printf("    %s\n", fingerprint);
        printf("\n");
        printf("  Chrome flag:\n");
        printf("    --ignore-certificate-errors-spki-list=sha256/%s\n", fingerprint);
        printf("\n");
        printf("================================================================\n");
        printf("\n");

        // Save to file
        FILE* fp = fopen("fingerprint.txt", "w");
        if (fp) {
            fprintf(fp, "sha256/%s\n", fingerprint);
            fclose(fp);
            printf("  Also saved to: fingerprint.txt\n\n");
        }
    }

    // Initialize wolfSSL
    wolfSSL_Init();

    // Create DTLS 1.2 server context
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    if (ctx == NULL) {
        printf("ERROR: Failed to create DTLS context\n");
        return 1;
    }

    // Load CA, server cert, server key
    printf("[1] Loading certificates...\n");
    ret = wolfSSL_CTX_load_verify_locations(ctx, "certs/client-cert.pem", 0);
    printf("    Load CA: %d (warn ok)\n", ret);

    ret = wolfSSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("    [ERROR] Load server cert: %d\n", ret);
        printf("    File: %s\n", cert_file);
        return 1;
    }
    printf("    [OK] Load server cert\n");

    ret = wolfSSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("    [ERROR] Load server key: %d\n", ret);
        printf("    File: %s\n", key_file);
        return 1;
    }
    printf("    [OK] Load server key\n");

    // Set cipher list - WebRTC compatible (ECDHE-RSA for RSA certificates)
    printf("[2] Setting cipher list...\n");
    ret = wolfSSL_CTX_set_cipher_list(ctx,
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-SHA256:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-SHA384");
    if (ret != WOLFSSL_SUCCESS) {
        printf("    [WARN] Failed to set cipher list: %d, using defaults\n", ret);
    } else {
        printf("    [OK] Cipher list set (ECDHE-RSA)\n");
    }

    // DTLS cookie for DoS protection
    // wolfSSL_CTX_set_dtls_cookies(ctx, NULL, NULL);

    // Set SRTP protection profiles (Chrome supports these)
    printf("[3] SRTP profiles...\n");
    // wolfSSL doesn't have built-in SRTP API like Pion's pion/dtls
    // We would need to use wolfSSL_GetCurrentCipherSrtp() after handshake
    printf("    [INFO] SRTP keys will be logged after handshake\n");

    // Create UDP socket
    printf("[4] Creating UDP socket on port %d...\n", SERV_PORT);
    int listenfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listenfd < 0) {
        printf("    [ERROR] socket() failed\n");
        wolfSSL_CTX_free(ctx);
        return 1;
    }

    struct sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(SERV_PORT);

    // Allow port reuse
    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));

    if (bind(listenfd, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        printf("    [ERROR] bind() failed\n");
        close_socket(listenfd);
        wolfSSL_CTX_free(ctx);
        return 1;
    }
    printf("    [OK] Socket bound to port %d\n", SERV_PORT);

    printf("\n[OK] Server ready. Waiting for DTLS connections...\n");
    printf("    Launch Chrome with:\n");
    printf("      chrome.exe --ignore-certificate-errors-spki-list=sha256/%s\n", fingerprint);
    printf("\n");

    // Wait for connections
    struct sockaddr_in cliaddr;
    socklen_t cliLen;
    unsigned char b[MSGLEN];
    char buff[MSGLEN];
    const char ack[] = "I hear you from wolfSSL!\n";
    WOLFSSL* ssl = NULL;
    int connection_count = 0;

    // Set timeout for recvfrom
    int timeout_ms = 500;
    setsockopt(listenfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    while (1) {
        cliLen = sizeof(cliaddr);
        memset(&cliaddr, 0, sizeof(cliaddr));
        memset(b, 0, sizeof(b));
        int bytesReceived = (int)recvfrom(listenfd, (char*)b, sizeof(b), 0,
                                          (struct sockaddr*)&cliaddr, &cliLen);

        if (bytesReceived == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == 10060 /* WSAETIMEDOUT */) {
                continue; // Loop and wait for next connection
            }
            printf("[ERR] recvfrom failed: %d\n", err);
            Sleep(100);
            continue;
        }

        if (bytesReceived == 0) {
            continue;
        }

        if (bytesReceived > 0) {
            printf("\n[CONNECT] Received %d bytes from peer\n", bytesReceived);

            // Connect UDP for bidirectional
            if (connect(listenfd, (struct sockaddr*)&cliaddr, sizeof(cliaddr)) != 0) {
                printf("    [ERROR] UDP connect failed\n");
                continue;
            }
            printf("    [OK] UDP connected\n");

            // Create SSL session
            ssl = wolfSSL_new(ctx);
            if (ssl == NULL) {
                printf("    [ERROR] wolfSSL_new failed\n");
                continue;
            }

            // Set file descriptor (for DTLS, after connect)
            wolfSSL_set_fd(ssl, listenfd);

            // Perform DTLS handshake
            printf("    [HANDSHAKE] Starting...\n");
            ret = wolfSSL_accept(ssl);
            if (ret != WOLFSSL_SUCCESS) {
                int e = wolfSSL_get_error(ssl, 0);
                char ebuf[256];
                wolfSSL_ERR_error_string(e, ebuf);
                printf("    [ERROR] DTLS handshake failed: %d, %s\n", e, ebuf);
                wolfSSL_free(ssl);
                ssl = NULL;
                close_socket(listenfd);
                listenfd = socket(AF_INET, SOCK_DGRAM, 0);
                on = 1;
                setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
                bind(listenfd, (struct sockaddr*)&servAddr, sizeof(servAddr));
                continue;
            }
            printf("    [SUCCESS] DTLS handshake complete!\n");

            // Show negotiated cipher
            WOLFSSL_CIPHER* cipher = wolfSSL_get_current_cipher(ssl);
            if (cipher) {
                printf("    Cipher: %s\n", wolfSSL_CIPHER_get_name(cipher));
            }

            connection_count++;
            printf("    Connection #%d completed.\n", connection_count);

            // Read/write a few messages
            int msg_count = 0;
            while (1) {
                memset(buff, 0, sizeof(buff));
                int recvLen = wolfSSL_read(ssl, buff, sizeof(buff)-1);

                if (recvLen > 0) {
                    buff[recvLen] = 0;
                    msg_count++;
                    printf("    [MSG #%d] %d bytes: %s\n", msg_count, recvLen, buff);

                    // Echo back
                    ret = wolfSSL_write(ssl, ack, sizeof(ack));
                    printf("    [SENT] %d bytes echo\n", ret);
                } else if (recvLen == 0) {
                    printf("    [CLOSE] Client sent close_notify\n");
                    break;
                } else {
                    int readErr = wolfSSL_get_error(ssl, 0);
                    if (readErr == WOLFSSL_ERROR_WANT_READ) {
                        continue;
                    }
                    printf("    [ERROR] Read failed: %d\n", readErr);
                    break;
                }
            }

            wolfSSL_shutdown(ssl);
            wolfSSL_free(ssl);
            ssl = NULL;
            close_socket(listenfd);

            // Recreate socket
            listenfd = socket(AF_INET, SOCK_DGRAM, 0);
            on = 1;
            setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
            bind(listenfd, (struct sockaddr*)&servAddr, sizeof(servAddr));

            printf("\n[READY] Listening for next connection...\n\n");
        }
    }

    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
