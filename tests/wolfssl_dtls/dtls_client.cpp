/**
 * @file dtls_client.cpp
 * @brief wolfSSL DTLS client to test the server
 *
 * This is a simple DTLS client that connects to our wolfSSL server
 * to verify handshake works.
 */

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define BUF_SIZE 4096
#define DTLS_MTU 1200

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    int port = 9999;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    printf("=== wolfSSL DTLS Client ===\n");
    printf("Connecting to %s:%d\n\n", host, port);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    wolfSSL_Init();
    wolfSSL_Debugging_ON();

    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfDTLSv1_2_client_method());
    if (!ctx) {
        printf("Failed to create CTX\n");
        return 1;
    }

    // Trust the server's self-signed cert
    wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    // Loading CA (optional)
    wolfSSL_CTX_load_verify_locations(ctx, "certs/client-cert.pem", 0);

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("socket() failed\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    // Connect UDP
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("connect() failed\n");
        return 1;
    }

    WOLFSSL* ssl = wolfSSL_new(ctx);
    if (!ssl) {
        printf("wolfSSL_new failed\n");
        return 1;
    }

    wolfSSL_set_fd(ssl, sock);

    printf("Starting DTLS handshake...\n");
    int ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        int e = wolfSSL_get_error(ssl, ret);
        char buf[256];
        wolfSSL_ERR_error_string(e, buf);
        printf("Handshake failed: %d, %s\n", e, buf);
    } else {
        printf("[SUCCESS] DTLS handshake completed!\n");
        WOLFSSL_CIPHER* cipher = wolfSSL_get_current_cipher(ssl);
        if (cipher) {
            printf("Cipher: %s\n", wolfSSL_CIPHER_get_name(cipher));
        }

        // Try to send a message
        const char* msg = "Hello from wolfSSL client!";
        ret = wolfSSL_write(ssl, msg, (int)strlen(msg));
        printf("Write: %d bytes sent\n", ret);

        // Try to read
        char buf[BUF_SIZE];
        ret = wolfSSL_read(ssl, buf, BUF_SIZE - 1);
        if (ret > 0) {
            buf[ret] = 0;
            printf("Read %d bytes: %s\n", ret, buf);
        }
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
