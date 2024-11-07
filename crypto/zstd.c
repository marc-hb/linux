// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cryptographic API.
 *
 * Copyright (c) 2017-present, Facebook, Inc.
 */
#include <linux/crypto.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/scompress.h>
#include <crypto/internal/acompress.h>


#define ZSTD_DEF_LEVEL	3
#define ZSTD_MAX_WINDOWLOG 18
#define ZSTD_MAX_INPUT_SIZE BIT(ZSTD_MAX_WINDOWLOG)
#define ZSTD_COMPRESS 0
#define ZSTD_DECOMPRESS 1

struct zstd_ctx {
	zstd_cctx *cctx;
	zstd_dctx *dctx;
	void *cwksp;
	void *dwksp;
	size_t wksp_size;
	zstd_parameters params;
};

static zstd_parameters zstd_params(void)
{
	return zstd_get_params(ZSTD_DEF_LEVEL, 0);
}

static int zstd_comp_init(struct zstd_ctx *ctx)
{
	int ret = 0;
	const zstd_parameters params = zstd_params();
	const size_t wksp_size = zstd_cctx_workspace_bound(&params.cParams);

	ctx->cwksp = vzalloc(wksp_size);
	if (!ctx->cwksp) {
		ret = -ENOMEM;
		goto out;
	}

	ctx->cctx = zstd_init_cctx(ctx->cwksp, wksp_size);
	if (!ctx->cctx) {
		ret = -EINVAL;
		goto out_free;
	}
out:
	return ret;
out_free:
	vfree(ctx->cwksp);
	goto out;
}

static int zstd_decomp_init(struct zstd_ctx *ctx)
{
	int ret = 0;
	const size_t wksp_size = zstd_dctx_workspace_bound();

	ctx->dwksp = vzalloc(wksp_size);
	if (!ctx->dwksp) {
		ret = -ENOMEM;
		goto out;
	}

	ctx->dctx = zstd_init_dctx(ctx->dwksp, wksp_size);
	if (!ctx->dctx) {
		ret = -EINVAL;
		goto out_free;
	}
out:
	return ret;
out_free:
	vfree(ctx->dwksp);
	goto out;
}

static void zstd_comp_exit(struct zstd_ctx *ctx)
{
	vfree(ctx->cwksp);
	ctx->cwksp = NULL;
	ctx->cctx = NULL;
}

static void zstd_decomp_exit(struct zstd_ctx *ctx)
{
	vfree(ctx->dwksp);
	ctx->dwksp = NULL;
	ctx->dctx = NULL;
}

static int __zstd_init(void *ctx)
{
	int ret;

	ret = zstd_comp_init(ctx);
	if (ret)
		return ret;
	ret = zstd_decomp_init(ctx);
	if (ret)
		zstd_comp_exit(ctx);
	return ret;
}

static int zstd_init(struct crypto_tfm *tfm)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	return __zstd_init(ctx);
}

static void __zstd_exit(void *ctx)
{
	zstd_comp_exit(ctx);
	zstd_decomp_exit(ctx);
}

static void zstd_exit(struct crypto_tfm *tfm)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	__zstd_exit(ctx);
}

static int __zstd_compress(const u8 *src, unsigned int slen,
			   u8 *dst, unsigned int *dlen, void *ctx)
{
	size_t out_len;
	struct zstd_ctx *zctx = ctx;
	const zstd_parameters params = zstd_params();

	out_len = zstd_compress_cctx(zctx->cctx, dst, *dlen, src, slen, &params);
	if (zstd_is_error(out_len))
		return -EINVAL;
	*dlen = out_len;
	return 0;
}

static int zstd_compress(struct crypto_tfm *tfm, const u8 *src,
			 unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	return __zstd_compress(src, slen, dst, dlen, ctx);
}

static int __zstd_decompress(const u8 *src, unsigned int slen,
			     u8 *dst, unsigned int *dlen, void *ctx)
{
	size_t out_len;
	struct zstd_ctx *zctx = ctx;

	out_len = zstd_decompress_dctx(zctx->dctx, dst, *dlen, src, slen);
	if (zstd_is_error(out_len))
		return -EINVAL;
	*dlen = out_len;
	return 0;
}

static int zstd_decompress(struct crypto_tfm *tfm, const u8 *src,
			   unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	return __zstd_decompress(src, slen, dst, dlen, ctx);
}

static int zstd_cstream_alloc_workspace(struct zstd_ctx *ctx)
{
	int ret = 0;

	ctx->params = zstd_get_params(ZSTD_DEF_LEVEL, ZSTD_MAX_INPUT_SIZE);

	ctx->wksp_size = max_t(size_t,
			       zstd_cstream_workspace_bound(&ctx->params.cParams),
			       zstd_dstream_workspace_bound(ZSTD_MAX_INPUT_SIZE));
	if (!ctx->wksp_size)
		return -EINVAL;

	ctx->cwksp = kvmalloc(ctx->wksp_size, GFP_KERNEL | __GFP_NOWARN);
	if (!ctx->cwksp) {
		ret = -ENOMEM;
		goto out_free;
	}

	ctx->dwksp = kvmalloc(ctx->wksp_size, GFP_KERNEL | __GFP_NOWARN);
	if (!ctx->dwksp) {
		ret = -ENOMEM;
		goto out_free;
	}

	return ret;

out_free:
	kvfree(ctx->cwksp);
	ctx->cwksp = NULL;
	kvfree(ctx->dwksp);
	ctx->dwksp = NULL;

	return ret;
}

static void zstd_cstream_exit(struct zstd_ctx *ctx)
{
	kvfree(ctx->cwksp);
	ctx->cwksp = NULL;
	zstd_free_cctx(ctx->cctx);
	ctx->cctx = NULL;
}

static void zstd_dstream_exit(struct zstd_ctx *ctx)
{
	kvfree(ctx->dwksp);
	ctx->dwksp = NULL;
	zstd_free_dctx(ctx->dctx);
	ctx->dctx = NULL;
}

static int zstd_cstream_init(struct zstd_ctx *ctx)
{
	int ret = 0;

	ctx->cctx = zstd_init_cstream(&ctx->params, 0, ctx->cwksp, ctx->wksp_size);
	if (!ctx->cctx)
		ret = -EINVAL;

	return ret;
}

static int zstd_dstream_init(struct zstd_ctx *ctx)
{
	int ret = 0;

	ctx->dctx = zstd_init_dstream(ZSTD_MAX_INPUT_SIZE, ctx->dwksp, ctx->wksp_size);
	if (!ctx->dctx)
		ret = -EINVAL;

	return ret;
}

static int zstd_acomp_init_tfm(struct crypto_acomp *acomp_tfm)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp_tfm);
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);
	int ret = 0;

	ret = zstd_cstream_alloc_workspace(ctx);
	if (ret)
		return ret;

	ret = zstd_cstream_init(ctx);
	if (ret)
		goto out;

	ret = zstd_dstream_init(ctx);
	if (ret)
		goto out;

	return ret;
out:
	zstd_cstream_exit(ctx);
	zstd_dstream_exit(ctx);

	return ret;
}

static void zstd_acomp_exit_tfm(struct crypto_acomp *acomp_tfm)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp_tfm);
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);

	zstd_cstream_exit(ctx);
	zstd_dstream_exit(ctx);
}

static int zstd_acomp_compress_decompress(struct acomp_req *req, int direction)
{
	struct crypto_acomp *acomp_tfm = crypto_acomp_reqtfm(req);
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp_tfm);
	struct zstd_ctx *ctx = crypto_tfm_ctx(tfm);
	unsigned int src_len = 0, dst_len = 0;
	struct scatterlist *sg_src, *sg_dst;
	bool decompression_done = false;
	bool compression_done = true;
	u8 *src_buff, *dst_buff;
	zstd_out_buffer outbuf;
	zstd_in_buffer inbuf;
	unsigned int pos = 0;
	size_t pending_bytes;
	size_t num_bytes;

	req->dlen = 0;

	sg_src = req->src;
	sg_dst = req->dst;
	src_buff = sg_virt(sg_src);
	dst_buff = sg_virt(sg_dst);
	src_len = sg_src->length;
	dst_len = sg_dst->length;

	while ((sg_src) && (sg_dst)) {
		inbuf.pos = 0;
		inbuf.src = src_buff;
		inbuf.size = src_len;

		outbuf.pos = 0;
		outbuf.dst = dst_buff;
		outbuf.size = dst_len;

		if (direction == ZSTD_COMPRESS) {
			num_bytes = zstd_compress_stream(ctx->cctx, &outbuf, &inbuf);
			if (ZSTD_isError(num_bytes))
				return -EIO;

			pending_bytes = zstd_flush_stream(ctx->cctx, &outbuf);
			if (ZSTD_isError(pending_bytes))
				return -EIO;

			if (pending_bytes > 0)
				compression_done = false;
			else
				compression_done = true;

		} else if (direction == ZSTD_DECOMPRESS) {
			pending_bytes = zstd_decompress_stream(ctx->dctx, &outbuf, &inbuf);
			if (ZSTD_isError(pending_bytes))
				return -EIO;

			if (pending_bytes > 0)
				decompression_done = false;
			else
				decompression_done = true;
		}

		if (inbuf.pos == src_len && (compression_done)) {
			sg_src = sg_next(sg_src);
			if (sg_src) {
				src_buff = sg_virt(sg_src);
				src_len = sg_src->length;
			}
		} else if (inbuf.pos < src_len) {
			src_buff += inbuf.pos;
			src_len -= inbuf.pos;
		}

		if (outbuf.pos == dst_len) {
			sg_dst = sg_next(sg_dst);
			if (sg_dst) {
				dst_buff = sg_virt(sg_dst);
				dst_len = sg_dst->length;
			}
		} else {
			dst_buff += outbuf.pos;
			dst_len -= outbuf.pos;
		}
		req->dlen += outbuf.pos;

		if (direction == ZSTD_DECOMPRESS) {
			if (decompression_done)
				break;
		}
	}

	if (direction == ZSTD_COMPRESS) {
		pos = outbuf.pos;
		zstd_end_stream(ctx->cctx, &outbuf);
		req->dlen += (outbuf.pos - pos);
	}

	return 0;
}

static int zstd_acomp_compress(struct acomp_req *req)
{
	return zstd_acomp_compress_decompress(req, ZSTD_COMPRESS);
}

static int zstd_acomp_decompress(struct acomp_req *req)
{
	return zstd_acomp_compress_decompress(req, ZSTD_DECOMPRESS);
}

static struct crypto_alg alg = {
	.cra_name		= "zstd",
	.cra_driver_name	= "zstd-generic",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct zstd_ctx),
	.cra_module		= THIS_MODULE,
	.cra_init		= zstd_init,
	.cra_exit		= zstd_exit,
	.cra_u			= { .compress = {
	.coa_compress		= zstd_compress,
	.coa_decompress		= zstd_decompress } }
};

static struct acomp_alg zstd_acomp = {
	.base = {
		.cra_name = "zstd",
		.cra_driver_name = "zstd_acomp",
		.cra_priority = 4001,
		.cra_flags = CRYPTO_ALG_ASYNC,
		.cra_ctxsize = sizeof(struct zstd_ctx),
		.cra_module = THIS_MODULE,
	},
	.init = zstd_acomp_init_tfm,
	.exit = zstd_acomp_exit_tfm,
	.compress = zstd_acomp_compress,
	.decompress = zstd_acomp_decompress,
	.dst_free = sgl_free,
	.reqsize = sizeof(struct zstd_ctx),
};

static int __init zstd_mod_init(void)
{
	int ret;

	ret = crypto_register_alg(&alg);
	if (ret)
		return ret;

	ret = crypto_register_acomp(&zstd_acomp);
	if (ret)
		crypto_unregister_alg(&alg);

	return ret;
}

static void __exit zstd_mod_fini(void)
{
	crypto_unregister_alg(&alg);
	crypto_unregister_acomp(&zstd_acomp);
}

subsys_initcall(zstd_mod_init);
module_exit(zstd_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zstd Compression Algorithm");
MODULE_ALIAS_CRYPTO("zstd");
