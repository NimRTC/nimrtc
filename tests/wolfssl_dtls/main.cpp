/**
 * @file main.cpp
 * @brief wolfSSL DTLS server for Chrome interop testing
 *
 * Build:
 *   mkdir build && cd build
 *   cmake -DWOLFSSL_DIR=/path/to/wolfssl ..
 *   cmake --build .
 *
 * Run:
 *   wolfssl_dtls_server
 *
 * Then in Chrome, open:
 *   data:text/html,<script>pc=new RTCPeerConnection();pc.createDataChannel("test");pc.createOffer().then(o=>pc.setLocalDescription(o));</script>
 *
 * With Chrome flags:
 *   chrome.exe --ignore-certificate-errors-spki-list=sha256/<fingerprint>
 */

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/settings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// Configuration
static const int SERVER_PORT = 9999;
static const int BUFFER_SIZE = 4096;
static const int TIMEOUT_MS = 5000;
static const int SRTP_KEY_LEN = 16;
static const int SRTP_SALT_LEN = 14;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_handshake_complete{false};
static std::atomic<bool> g_dtls_connected{false};

// Save generated fingerprint
static std::string g_fingerprint_b64;

// -----------------------------------------------------------------------------
// Utility: Base64 encoding
// -----------------------------------------------------------------------------

static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char* b64_table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;

    for (size_t i = 0; i < len; i += 3) {
        size_t remaining = len - i;
        uint8_t b0 = data[i];
        uint8_t b1 = (remaining > 1) ? data[i + 1] : 0;
        uint8_t b2 = (remaining > 2) ? data[i + 2] : 0;

        result += b64_table[(b0 >> 2) & 0x3F];
        result += b64_table[((b0 << 4) | (b1 >> 4)) & 0x3F];

        if (remaining > 1) {
            result += b64_table[((b1 << 2) | (b2 >> 6)) & 0x3F];
        } else {
            result += '=';
        }

        if (remaining > 2) {
            result += b64_table[b2 & 0x3F];
        } else {
            result += '=';
        }
    }
    return result;
}

// -----------------------------------------------------------------------------
// Utility: Hex dump
// -----------------------------------------------------------------------------

static void hex_dump(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 8 == 0) printf(" ");
    }
    if (len % 16 != 0) printf("\n");
}

// -----------------------------------------------------------------------------
// Generate self-signed ECDSA P-256 certificate and print fingerprint
// -----------------------------------------------------------------------------

static int generate_certificate_fingerprint(uint8_t* fingerprint, size_t* fp_len) {
    int ret;
    WC_RNG rng;
    ecc_key key;
    uint8_t der[512];
    word32 der_len = sizeof(der);
    uint8_t hash[32];
    Sha256 sha;

    printf("[INFO] Generating ECDSA P-256 key pair...\n");

    ret = wc_InitRng(&rng);
    if (ret != 0) {
        printf("[ERROR] wc_InitRng() failed: %d\n", ret);
        return ret;
    }

    ret = wc_ecc_init(&key);
    if (ret != 0) {
        printf("[ERROR] wc_ecc_init() failed: %d\n", ret);
        wc_FreeRng(&rng);
        return ret;
    }

    ret = wc_ecc_make_key(&rng, 32, &key);
    if (ret != 0) {
        printf("[ERROR] wc_ecc_make_key() failed: %d\n", ret);
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        return ret;
    }

    // Export public key as DER (SubjectPublicKeyInfo format)
    ret = wc_ecc_export der(&key, der, &der_len, NULL, 0);
    if (ret != 0) {
        printf("[ERROR] wc_ecc_export_der() failed: %d\n", ret);
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        return ret;
    }

    // Compute SHA-256 of the DER
    ret = wc_InitSha256(&sha);
    if (ret != 0) {
        printf("[ERROR] wc_InitSha256() failed: %d\n", ret);
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        return ret;
    }

    ret = wc_Sha256Update(&sha, der, der_len);
    if (ret != 0) {
        printf("[ERROR] wc_Sha256Update() failed: %d\n", ret);
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        return ret;
    }

    ret = wc_Sha256Final(&sha, hash);
    if (ret != 0) {
        printf("[ERROR] wc_Sha256Final() failed: %d\n", ret);
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        return ret;
    }

    // Copy fingerprint
    memcpy(fingerprint, hash, 32);
    *fp_len = 32;

    // Print fingerprint
    std::string hex_fp;
    for (int i = 0; i < 32; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", hash[i]);
        hex_fp += buf;
        if (i < 31) hex_fp += ":";
    }

    g_fingerprint_b64 = base64_encode(hash, 32);

    printf("\n");
    printf("========================================\n");
    printf("  wolfSSL ECDSA P-256 Certificate\n");
    printf("========================================\n");
    printf("\n");
    printf("SPKI DER length: %u bytes\n", der_len);
    printf("\n");
    printf("SHA-256 (RFC 8122 format):\n");
    printf("  %s\n", hex_fp.c_str());
    printf("\n");
    printf("Base64:\n");
    printf("  %s\n", g_fingerprint_b64.c_str());
    printf("\n");
    printf("Chrome flag:\n");
    printf("  --ignore-certificate-errors-spki-list=sha256/%s\n", g_fingerprint_b64.c_str());
    printf("\n");
    printf("========================================\n");

    // Save fingerprint to file for automation
    {
        std::ofstream fp_file("build/fingerprint.txt");
        fp_file << "sha256/" << g_fingerprint_b64 << std::endl;
        fp_file.close();
    }

    wc_ecc_free(&key);
    wc_FreeRng(&rng);

    return 0;
}

// -----------------------------------------------------------------------------
// Initialize Winsock
// -----------------------------------------------------------------------------

static int init_winsock() {
    WSADATA wsa_data;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (ret != 0) {
        printf("[ERROR] WSAStartup failed with %d\n", ret);
        return -1;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Create UDP socket
// -----------------------------------------------------------------------------

static SOCKET create_udp_socket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("[ERROR] socket() failed with %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }

    // Set receive timeout
    DWORD timeout = 100;  // 100ms for polling
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout,
                    sizeof(timeout)) < 0) {
        printf("[WARNING] setsockopt(SO_RCVTIMEO) failed\n");
    }

    // Allow broadcast
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast,
                    sizeof(broadcast)) < 0) {
        printf("[WARNING] setsockopt(SO_BROADCAST) failed\n");
    }

    return sock;
}

// -----------------------------------------------------------------------------
// Create DTLS context
// -----------------------------------------------------------------------------

static WOLFSSL_CTX* create_dtls_context() {
    int ret;

    // Initialize wolfSSL
    ret = wolfSSL_Init();
    if (ret != WOLFSSL_SUCCESS) {
        printf("[ERROR] wolfSSL_Init() failed\n");
        return nullptr;
    }

    // Create context - use server method for DTLS
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    if (!ctx) {
        printf("[ERROR] wolfSSL_CTX_new() failed\n");
        wolfSSL_Cleanup();
        return nullptr;
    }

    // Load certificates for testing
    // Note: wolfSSL certificates are in the source distribution
    const char* cert_file = "../certs/client-cert.pem";
    const char* key_file = "../certs/client-key.pem";

    ret = wolfSSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[WARNING] Failed to load certificate: %s\n", cert_file);
        printf("[INFO] Server will continue without certificate (for testing only)\n");
    } else {
        printf("[INFO] Certificate loaded successfully\n");
    }

    ret = wolfSSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[WARNING] Failed to load private key: %s\n", key_file);
    } else {
        printf("[INFO] Private key loaded successfully\n");
    }

    // Set cipher list - WebRTC compatible
    const char* cipher_list =
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES128-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES256-SHA384";
    ret = wolfSSL_CTX_set_cipher_list(ctx, cipher_list);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[WARNING] wolfSSL_CTX_set_cipher_list() failed\n");
    }

    // Enable Extended Master Secret
    ret = wolfSSL_CTX_UseExtendedMasterSecret(ctx, 1);
    if (ret != WOLFSSL_SUCCESS) {
        printf("[WARNING] wolfSSL_CTX_UseExtendedMasterSecret() failed\n");
    }

    // Set minimum DH key size
    wolfSSL_CTX_set_minDhKey_Sz(ctx, 128);

    return ctx;
}

// -----------------------------------------------------------------------------
// Signal handler
// -----------------------------------------------------------------------------

static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        printf("\n[INFO] Shutting down...\n");
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    printf("\n");
    printf("========================================\n");
    printf("  wolfSSL DTLS Server for Chrome Interop\n");
    printf("========================================\n");
    printf("\n");

    // Parse command line
    int port = SERVER_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-p port]\n", argv[0]);
            return 0;
        }
    }
    printf("[INFO] Server port: %d\n", port);

    // Set up signal handler
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    // Generate and print fingerprint
    uint8_t fingerprint[32];
    size_t fp_len = 0;
    if (generate_certificate_fingerprint(fingerprint, &fp_len) != 0) {
        printf("[ERROR] Failed to generate certificate\n");
        return 1;
    }

    // Initialize Winsock
    if (init_winsock() != 0) {
        return 1;
    }

    // Create UDP socket
    SOCKET sock = create_udp_socket();
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[ERROR] bind() failed with %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("[INFO] Socket bound to port %d\n", port);

    // Create DTLS context
    WOLFSSL_CTX* ctx = create_dtls_context();
    if (!ctx) {
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("\n[INFO] Waiting for DTLS connections...\n");
    printf("[INFO] Press Ctrl+C to exit\n");
    printf("[INFO] Run Chrome with the fingerprint flag shown above\n\n");

    uint8_t buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);
    WOLFSSL* ssl = nullptr;
    int handshake_attempts = 0;
    const int max_handshake_attempts = 10;

    while (g_running) {
        // Set up client address
        memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sin_family = AF_INET;
        client_addr.sin_port = htons(0);
        client_addr.sin_addr.s_addr = INADDR_ANY;

        // Receive data
        int n = recvfrom(sock, (char*)buffer, BUFFER_SIZE, 0,
                         (struct sockaddr*)&client_addr, &addr_len);

        if (n > 0) {
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            printf("\n[INFO] Received %d bytes from %s:%d\n",
                   n, client_ip, ntohs(client_addr.sin_port));

            // Print DTLS record header
            if (n >= 13) {
                uint8_t content_type = buffer[0];
                uint8_t major = buffer[1];
                uint8_t minor = buffer[2];
                uint16_t epoch = (buffer[3] << 8) | buffer[4];
                uint64_t seq = 0;
                for (int i = 0; i < 6; i++) {
                    seq = (seq << 8) | buffer[5 + i];
                }
                uint16_t length = (buffer[11] << 8) | buffer[12];

                printf("[DEBUG] DTLS record: type=%u, version=%u.%u, epoch=%u, seq=%llu, length=%u\n",
                       content_type, major, minor, epoch, seq, length);

                // Print handshake header if present
                if (n >= 25 && content_type == 22) {
                    uint8_t hs_type = buffer[13];
                    uint32_t hs_len = (buffer[16] << 16) | (buffer[17] << 8) | buffer[18];
                    uint16_t msg_seq = (buffer[19] << 8) | buffer[20];
                    printf("[DEBUG] Handshake: type=%u, length=%u, seq=%u\n",
                           hs_type, hs_len, msg_seq);
                }
            }

            // Create new SSL session for new connection
            if (!ssl) {
                ssl = wolfSSL_new(ctx);
                if (!ssl) {
                    printf("[ERROR] wolfSSL_new() failed\n");
                    continue;
                }

                // Set I/O callbacks for DTLS
                wolfSSL_SetIOReadCtx(ssl, &sock);
                wolfSSL_SetIOWriteCtx(ssl, &sock);
            }

            // Feed data to wolfSSL
            ret = wolfSSL_DTLS(ssl);
            if (ret != WOLFSSL_SUCCESS) {
                printf("[DEBUG] Setting DTLS mode\n");
            }

            // Accept connection
            ret = wolfSSL_accept(ssl);
            if (ret != WOLFSSL_SUCCESS) {
                int err = wolfSSL_get_error(ssl, ret);
                if (err == WOLFSSL_ERROR_WANT_READ ||
                    err == WOLFSSL_ERROR_WANT_WRITE) {
                    // Need more data
                    handshake_attempts++;
                    if (handshake_attempts <= 3) {
                        printf("[DEBUG] DTLS handshake in progress (attempt %d)...\n",
                               handshake_attempts);
                    }
                    continue;
                } else if (err == WOLFSSL_FATAL_ERROR) {
                    char err_buf[256];
                    wolfSSL_ERR_error_string(err, err_buf);
                    printf("[ERROR] DTLS accept failed: %s (err=%d)\n", err_buf, err);
                    wolfSSL_free(ssl);
                    ssl = nullptr;
                    handshake_attempts = 0;
                    continue;
                }
            }

            // Handshake complete
            if (!g_dtls_connected) {
                g_dtls_connected = true;
                printf("\n");
                printf("========================================\n");
                printf("  DTLS Handshake Complete!\n");
                printf("========================================\n");
                printf("\n");

                // Get cipher info
                const WOLFSSL_CIPHER* cipher = wolfSSL_get_current_cipher(ssl);
                if (cipher) {
                    printf("[INFO] Negotiated cipher: %s\n", wolfSSL_CIPHER_get_name(cipher));
                }

                // Get key exchange info
                int key_size = wolfSSL_get_key_size(ssl);
                printf("[INFO] Key size: %d bits\n", key_size * 8);

                // Get SRTP profile
                int srtp_profile = wolfSSL_get_srtp_keys(ssl, NULL, NULL, NULL, NULL);
                if (srtp_profile != 0) {
                    printf("[INFO] SRTP profile negotiated: %d\n", srtp_profile);
                }

                printf("\n[SUCCESS] wolfSSL <-> Chrome DTLS interop verified!\n");
                printf("\n");
            }
        } else if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                // Timeout - normal, just continue polling
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                printf("[ERROR] recvfrom() failed with %d\n", err);
            }
        }

        // Check if handshake timed out
        if (!g_dtls_connected && handshake_attempts > max_handshake_attempts) {
            printf("[WARNING] DTLS handshake timeout after %d attempts\n",
                   max_handshake_attempts);
            printf("[INFO] Make sure Chrome is sending DTLS ClientHello\n");
            if (ssl) {
                wolfSSL_free(ssl);
                ssl = nullptr;
            }
            handshake_attempts = 0;
        }
    }

    // Cleanup
    printf("[INFO] Cleaning up...\n");
    if (ssl) {
        wolfSSL_free(ssl);
    }
    if (ctx) {
        wolfSSL_CTX_free(ctx);
    }
    closesocket(sock);
    wolfSSL_Cleanup();
    WSACleanup();

    printf("[INFO] Server stopped.\n");
    return 0;
}
