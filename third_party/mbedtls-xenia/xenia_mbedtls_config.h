// Mbed TLS modules xenia builds. Each one needs its library/*.c file listed in
// the mbedtls target in third_party/CMakeLists.txt.
#ifndef XENIA_MBEDTLS_CONFIG_H_
#define XENIA_MBEDTLS_CONFIG_H_

#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_DES_C
#define MBEDTLS_DHM_C
// ECDSA P-256 signs the online service's device keys (net/live_account).
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_MD5_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

#endif  // XENIA_MBEDTLS_CONFIG_H_
