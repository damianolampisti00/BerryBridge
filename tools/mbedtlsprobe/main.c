/* Diagnostic BB10 TLS 1.2 probe. Uses the same mbedTLS 2.28.9 build as BBport. */
#include <stdio.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "www.cloudflare.com";
    const char *port = argc > 2 ? argv[2] : "443";
    const char *ca_file = argc > 3 ? argv[3] : "cacert.pem";
    int ret = 1;
    int rc;
    char error[256];
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;

    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca);

    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)"BerryBridge TLS probe", 21);
    if (rc != 0) goto done;
    rc = mbedtls_x509_crt_parse_file(&ca, ca_file);
    if (rc < 0) goto done;
    rc = mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) goto done;
    rc = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) goto done;
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &ca, NULL);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
    rc = mbedtls_ssl_setup(&ssl, &config);
    if (rc != 0) goto done;
    rc = mbedtls_ssl_set_hostname(&ssl, host);
    if (rc != 0) goto done;
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto done;
    }
    if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
        mbedtls_x509_crt_verify_info(error, sizeof(error), "", mbedtls_ssl_get_verify_result(&ssl));
        fprintf(stderr, "certificate verification failed: %s\n", error);
        goto cleanup;
    }
    printf("verified TLS %s, cipher %s, host %s\n",
           mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl), host);
    ret = 0;
    goto cleanup;

done:
    mbedtls_strerror(rc, error, sizeof(error));
    fprintf(stderr, "mbedTLS probe failed (%d): %s\n", rc, error);
cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&net);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}
