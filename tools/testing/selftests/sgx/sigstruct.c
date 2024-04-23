// SPDX-License-Identifier: GPL-2.0
/*  Copyright(c) 2016-20 Intel Corporation. */

#define _GNU_SOURCE
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include "defines.h"
#include "main.h"

/*
 * FIXME: OpenSSL 3.0 has deprecated some functions. For now just ignore
 * the warnings.
 */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

struct q1q2_ctx {
	BN_CTX *bn_ctx;
	BIGNUM *m;
	BIGNUM *s;
	BIGNUM *q1;
	BIGNUM *qr;
	BIGNUM *q2;
};

static void free_q1q2_ctx(struct q1q2_ctx *ctx)
{
	BN_CTX_free(ctx->bn_ctx);
	BN_free(ctx->m);
	BN_free(ctx->s);
	BN_free(ctx->q1);
	BN_free(ctx->qr);
	BN_free(ctx->q2);
}

static bool alloc_q1q2_ctx(const uint8_t *s, const uint8_t *m,
			   struct q1q2_ctx *ctx)
{
	ctx->bn_ctx = BN_CTX_new();
	ctx->s = BN_bin2bn(s, SGX_MODULUS_SIZE, NULL);
	ctx->m = BN_bin2bn(m, SGX_MODULUS_SIZE, NULL);
	ctx->q1 = BN_new();
	ctx->qr = BN_new();
	ctx->q2 = BN_new();

	if (!ctx->bn_ctx || !ctx->s || !ctx->m || !ctx->q1 || !ctx->qr ||
	    !ctx->q2) {
		free_q1q2_ctx(ctx);
		return false;
	}

	return true;
}

static void reverse_bytes(void *data, int length)
{
	int i = 0;
	int j = length - 1;
	uint8_t temp;
	uint8_t *ptr = data;

	while (i < j) {
		temp = ptr[i];
		ptr[i] = ptr[j];
		ptr[j] = temp;
		i++;
		j--;
	}
}

static bool calc_q1q2(const uint8_t *s, const uint8_t *m, uint8_t *q1,
		      uint8_t *q2)
{
	struct q1q2_ctx ctx;
	int len;

	if (!alloc_q1q2_ctx(s, m, &ctx)) {
		fprintf(stderr, "Not enough memory for Q1Q2 calculation\n");
		return false;
	}

	if (!BN_mul(ctx.q1, ctx.s, ctx.s, ctx.bn_ctx))
		goto out;

	if (!BN_div(ctx.q1, ctx.qr, ctx.q1, ctx.m, ctx.bn_ctx))
		goto out;

	if (BN_num_bytes(ctx.q1) > SGX_MODULUS_SIZE) {
		fprintf(stderr, "Too large Q1 %d bytes\n",
			BN_num_bytes(ctx.q1));
		goto out;
	}

	if (!BN_mul(ctx.q2, ctx.s, ctx.qr, ctx.bn_ctx))
		goto out;

	if (!BN_div(ctx.q2, NULL, ctx.q2, ctx.m, ctx.bn_ctx))
		goto out;

	if (BN_num_bytes(ctx.q2) > SGX_MODULUS_SIZE) {
		fprintf(stderr, "Too large Q2 %d bytes\n",
			BN_num_bytes(ctx.q2));
		goto out;
	}

	len = BN_bn2bin(ctx.q1, q1);
	reverse_bytes(q1, len);
	len = BN_bn2bin(ctx.q2, q2);
	reverse_bytes(q2, len);

	free_q1q2_ctx(&ctx);
	return true;
out:
	free_q1q2_ctx(&ctx);
	return false;
}

struct sgx_sigstruct_payload {
	struct sgx_sigstruct_header header;
	struct sgx_sigstruct_body body;
};

static bool check_crypto_errors(void)
{
	int err;
	bool had_errors = false;
	const char *filename;
	int line;
	char str[256];

	for ( ; ; ) {
		if (ERR_peek_error() == 0)
			break;

		had_errors = true;
		err = ERR_get_error_line(&filename, &line);
		ERR_error_string_n(err, str, sizeof(str));
		fprintf(stderr, "crypto: %s: %s:%d\n", str, filename, line);
	}

	return had_errors;
}

static inline const BIGNUM *get_modulus(RSA *key)
{
	const BIGNUM *n;

	RSA_get0_key(key, &n, NULL, NULL);
	return n;
}

static RSA *gen_sign_key(void)
{
	unsigned long sign_key_length;
	BIO *bio;
	RSA *key;

	sign_key_length = (unsigned long)&sign_key_end -
			  (unsigned long)&sign_key;

	bio = BIO_new_mem_buf(&sign_key, sign_key_length);
	if (!bio)
		return NULL;

	key = PEM_read_bio_RSAPrivateKey(bio, NULL, NULL, NULL);
	BIO_free(bio);

	return key;
}

enum mrtags {
	MRECREATE = 0x0045544145524345,
	MREADD = 0x0000000044444145,
	MREEXTEND = 0x00444E4554584545,
};

static bool mrenclave_update(EVP_MD_CTX *ctx, const void *data, size_t size)
{
	if (!EVP_DigestUpdate(ctx, data, size)) {
		fprintf(stderr, "digest update failed\n");
		return false;
	}

	return true;
}

static bool mrenclave_commit(EVP_MD_CTX *ctx, uint8_t *mrenclave)
{
	unsigned int size;

	if (!EVP_DigestFinal_ex(ctx, (unsigned char *)mrenclave, &size)) {
		fprintf(stderr, "digest commit failed\n");
		return false;
	}

	if (size != SHA256_DIGEST_LENGTH && size != SHA384_DIGEST_LENGTH) {
		fprintf(stderr, "invalid digest size = %u\n", size);
		return false;
	}

	return true;
}

struct mrecreate {
	uint64_t tag;
	uint32_t ssaframesize;
	uint64_t size;
	uint8_t reserved[44]; /* Padding to 64 */
} __attribute__((__packed__));

struct mrecreate384 {
	uint64_t tag;
	uint32_t ssaframesize;
	uint64_t size;
	uint8_t reserved[108]; /* Padding to 128 */
} __attribute__((__packed__));

static bool mrenclave_ecreate(EVP_MD_CTX *ctx, uint64_t blob_size,
			      enum sgx_sighashtype hash_type)
{
	const EVP_MD *type = hash_type == SGX_SIGHASHTYPE_SHA256 ?
				     EVP_sha256() :
				     EVP_sha384();
	struct mrecreate384 mrecreate384;
	struct mrecreate mrecreate;
	uint64_t encl_size;

	for (encl_size = 0x1000; encl_size < blob_size; )
		encl_size <<= 1;

	memset(&mrecreate, 0, sizeof(mrecreate));
	mrecreate.tag = MRECREATE;
	mrecreate.ssaframesize = 1;
	mrecreate.size = encl_size;

	if (!EVP_DigestInit_ex(ctx, type, NULL))
		return false;

	if (hash_type == SGX_SIGHASHTYPE_SHA384) {
		memset(&mrecreate384, 0, sizeof(mrecreate384));
		memcpy(&mrecreate384, &mrecreate, sizeof(mrecreate));
		return mrenclave_update(ctx, &mrecreate384, sizeof(mrecreate384));
	}

	return mrenclave_update(ctx, &mrecreate, sizeof(mrecreate));
}

struct mreadd {
	uint64_t tag;
	uint64_t offset;
	uint64_t flags; /* SECINFO flags */
	uint8_t reserved[40]; /* Padding to 64 */
} __attribute__((__packed__));

struct mreadd384 {
	uint64_t tag;
	uint64_t offset;
	uint64_t flags; /* SECINFO flags */
	uint8_t reserved[104]; /* Padding to 128 */
} __attribute__((__packed__));

static bool mrenclave_eadd(EVP_MD_CTX *ctx, uint64_t offset, uint64_t flags,
			   enum sgx_sighashtype hash_type)
{
	struct mreadd384 mreadd384;
	struct mreadd mreadd;

	memset(&mreadd, 0, sizeof(mreadd));
	mreadd.tag = MREADD;
	mreadd.offset = offset;
	mreadd.flags = flags;

	if (hash_type == SGX_SIGHASHTYPE_SHA384) {
		memset(&mreadd384, 0, sizeof(mreadd384));
		memcpy(&mreadd384, &mreadd, sizeof(mreadd));
		return mrenclave_update(ctx, &mreadd384, sizeof(mreadd384));
	}

	return mrenclave_update(ctx, &mreadd, sizeof(mreadd));
}

struct mreextend {
	uint64_t tag;
	uint64_t offset;
	uint8_t reserved[48]; /* Padding to 64 */
} __attribute__((__packed__));

struct mreextend384 {
	uint64_t tag;
	uint64_t offset;
	uint8_t reserved[112]; /* Padding to 128 */
} __attribute__((__packed__));

static bool mrenclave_eextend(EVP_MD_CTX *ctx, uint64_t offset,
			      const uint8_t *data, enum sgx_sighashtype hash_type)
{
	struct mreextend384 mreextend384;
	struct mreextend mreextend;
	size_t size = hash_type ? 128 : 64;
	int i;

	for (i = 0; i < 0x1000; i += 0x100) {
		memset(&mreextend, 0, sizeof(mreextend));
		mreextend.tag = MREEXTEND;
		mreextend.offset = offset + i;

		if (hash_type == SGX_SIGHASHTYPE_SHA384) {
			memset(&mreextend384, 0, sizeof(mreextend384));
			memcpy(&mreextend384, &mreextend, sizeof(mreextend));
			if (!mrenclave_update(ctx, &mreextend384, sizeof(mreextend384)))
				return false;
		} else {
			if (!mrenclave_update(ctx, &mreextend, sizeof(mreextend)))
				return false;
		}

		if (!mrenclave_update(ctx, &data[i + 0x00], size))
			return false;

		if (hash_type == SGX_SIGHASHTYPE_SHA256) {
			if (!mrenclave_update(ctx, &data[i + 0x40], size))
				return false;
		}

		if (!mrenclave_update(ctx, &data[i + 0x80], size))
			return false;

		if (hash_type == SGX_SIGHASHTYPE_SHA256) {
			if (!mrenclave_update(ctx, &data[i + 0xC0], size))
				return false;
		}
	}

	return true;
}

static bool mrenclave_segment(EVP_MD_CTX *ctx, struct encl *encl,
			      struct encl_segment *seg)
{
	uint64_t end = seg->size;
	uint64_t offset;

	for (offset = 0; offset < end; offset += PAGE_SIZE) {
		if (!mrenclave_eadd(ctx, seg->offset + offset, seg->flags,
				    encl->sigstruct.header.sighashtype))
			return false;

		if (seg->measure) {
			if (!mrenclave_eextend(ctx, seg->offset + offset, seg->src + offset,
					       encl->sigstruct.header.sighashtype))
				return false;
		}
	}

	return true;
}

bool encl_measure(struct encl *encl, struct opt_in *opt_param)
{
	uint64_t header1[2] = {0x000000E100000006, 0x0000000000010000};
	uint64_t header2[2] = {0x0000006000000101, 0x0000000100000060};
	struct sgx_sigstruct *sigstruct = &encl->sigstruct;
	struct sgx_sigstruct_payload payload;
	uint8_t digest[SHA384_DIGEST_LENGTH];
	unsigned int siglen;
	RSA *key = NULL;
	EVP_MD_CTX *ctx = NULL;
	int digest_len;
	int sha_type;
	int i;

	memset(sigstruct, 0, sizeof(*sigstruct));

	sigstruct->header.header1[0] = header1[0];
	sigstruct->header.header1[1] = header1[1];
	sigstruct->header.header2[0] = header2[0];
	sigstruct->header.header2[1] = header2[1];
	sigstruct->exponent = 3;
	sigstruct->body.attributes = SGX_ATTR_MODE64BIT;
	sigstruct->body.xfrm = 3;
	if (opt_param) {
		sigstruct->header.sighashtype = opt_param->sighashtype;
		sigstruct->body.attributes |= opt_param->body_attributes;
	}

	/* sanity check */
	if (check_crypto_errors())
		goto err;

	key = gen_sign_key();
	if (!key) {
		ERR_print_errors_fp(stdout);
		goto err;
	}

	BN_bn2bin(get_modulus(key), sigstruct->modulus);

	ctx = EVP_MD_CTX_create();
	if (!ctx)
		goto err;

	if (!mrenclave_ecreate(ctx, encl->src_size, sigstruct->header.sighashtype))
		goto err;

	for (i = 0; i < encl->nr_segments; i++) {
		struct encl_segment *seg = &encl->segment_tbl[i];

		if (!mrenclave_segment(ctx, encl, seg))
			goto err;
	}

	if (!mrenclave_commit(ctx, sigstruct->body.mrenclave))
		goto err;

	memcpy(&payload.header, &sigstruct->header, sizeof(sigstruct->header));
	memcpy(&payload.body, &sigstruct->body, sizeof(sigstruct->body));

	if (sigstruct->header.sighashtype == SGX_SIGHASHTYPE_SHA256) {
		SHA256((unsigned char *)&payload, sizeof(payload), digest);
		sha_type = NID_sha256;
		digest_len = SHA256_DIGEST_LENGTH;
	} else {
		SHA384((unsigned char *)&payload, sizeof(payload), digest);
		sha_type = NID_sha384;
		digest_len = SHA384_DIGEST_LENGTH;
	}

	if (!RSA_sign(sha_type, digest, digest_len,
		      sigstruct->signature, &siglen, key))
		goto err;

	if (!calc_q1q2(sigstruct->signature, sigstruct->modulus, sigstruct->q1,
		       sigstruct->q2))
		goto err;

	/* BE -> LE */
	reverse_bytes(sigstruct->signature, SGX_MODULUS_SIZE);
	reverse_bytes(sigstruct->modulus, SGX_MODULUS_SIZE);

	EVP_MD_CTX_destroy(ctx);
	RSA_free(key);
	return true;

err:
	if (ctx)
		EVP_MD_CTX_destroy(ctx);
	RSA_free(key);
	return false;
}
