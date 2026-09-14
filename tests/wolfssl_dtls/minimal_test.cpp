/**
 * @file minimal_test.cpp
 * @brief Minimal wolfSSL DTLS test - multiple approaches
 */

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int ret;
    WOLFSSL_CERT_MANAGER* cm = NULL;
    WOLFSSL* ssl = NULL;
    int err;

    printf("=== wolfSSL Multi-Approach Test ===\n\n");

    ret = wolfSSL_Init();
    printf("[1] wolfSSL_Init = %d\n", ret);

    /* Approach 1: Try creating session without ctx setup issues */
    {
        WOLFSSL_METHOD* method = wolfDTLSv1_2_server_method();
        printf("[2] wolfDTLSv1_2_server_method = %p\n", (void*)method);

        WOLFSSL_CTX* ctx = wolfSSL_CTX_new(method);
        printf("[3] wolfSSL_CTX_new = %p\n", (void*)ctx);

        if (ctx) {
            /* Disable peer verification for testing */
            wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

            /* Try to load test certs from wolfSSL */
            ret = wolfSSL_CTX_use_certificate_file(ctx, "../certs/server-cert.pem", SSL_FILETYPE_PEM);
            printf("[4] Load server cert = %d (0=success)\n", ret);

            ret = wolfSSL_CTX_use_PrivateKey_file(ctx, "../certs/server-key.pem", SSL_FILETYPE_PEM);
            printf("[5] Load server key = %d (0=success)\n", ret);

            /* Don't set cipher list - use defaults */

            ssl = wolfSSL_new(ctx);
            printf("[6] wolfSSL_new = %p\n", (void*)ssl);

            if (ssl) {
                printf("    [SUCCESS] wolfSSL DTLS API works\n");
                printf("    Version: %s\n", wolfSSL_lib_version());
                wolfSSL_free(ssl);
                ssl = NULL;
            } else {
                err = wolfSSL_get_error(ssl, 0);
                printf("    Error code: %d\n", err);
                char buf[80];
                wolfSSL_ERR_error_string(err, buf);
                printf("    Error: %s\n", buf);
            }

            wolfSSL_CTX_free(ctx);
        }
    }

    /* Approach 2: Pre-load default certs before wolfSSL_new */
    if (ssl == NULL) {
        printf("\n[Trying Approach 2: With system certs]\n");

        WOLFSSL_METHOD* method = wolfDTLSv1_2_server_method();
        WOLFSSL_CTX* ctx = wolfSSL_CTX_new(method);

        if (ctx) {
            wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
            wolfSSL_CTX_load_verify_buffer(ctx, NULL, 0, SSL_FILETYPE_PEM);

            /* Load wolfSSL test certs */
            ret = wolfSSL_CTX_use_certificate_file(ctx, "certs/server-cert.pem", SSL_FILETYPE_PEM);
            printf("    Load cert: %d\n", ret);

            ret = wolfSSL_CTX_use_PrivateKey_file(ctx, "certs/server-key.pem", SSL_FILETYPE_PEM);
            printf("    Load key: %d\n", ret);

            ssl = wolfSSL_new(ctx);
            printf("    wolfSSL_new = %p\n", (void*)ssl);

            if (ssl) {
                printf("    [SUCCESS]\n");
                wolfSSL_free(ssl);
            } else {
                err = wolfSSL_get_error(ssl, 0);
                printf("    Error: %d\n", err);
            }

            wolfSSL_CTX_free(ctx);
        }
    }

    /* Cleanup */
    if (cm) wolfSSL_CertManagerFree(cm);
    wolfSSL_Cleanup();

    printf("\n=== Test Complete ===\n");
    return ssl ? 0 : 1;
}
