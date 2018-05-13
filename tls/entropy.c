/*
 *  Entropy accumulator implementation
 *
 *  Copyright (C) 2006-2016, ARM Limited, All Rights Reserved
 *  Copyright (C) 2015-2018 Tempesta Technologies, Inc.
 *  SPDX-License-Identifier: GPL-2.0
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */
#if defined(MBEDTLS_ENTROPY_C)

#include "entropy.h"
#include "entropy_poll.h"

#include <string.h>

#if defined(MBEDTLS_HAVEGE_C)
#include "havege.h"
#endif

/* Implementation that should never be optimized out by the compiler */
static void mbedtls_zeroize(void *v, size_t n) {
	volatile unsigned char *p = v; while (n--) *p++ = 0;
}

#define ENTROPY_MAX_LOOP	256	 /**< Maximum amount to loop before error */

void mbedtls_entropy_init(mbedtls_entropy_context *ctx)
{
	ctx->source_count = 0;
	memset(ctx->source, 0, sizeof(ctx->source));

	spin_lock_init(&ctx->mutex);

	ctx->accumulator_started = 0;
#if defined(MBEDTLS_ENTROPY_SHA512_ACCUMULATOR)
	mbedtls_sha512_init(&ctx->accumulator);
#else
	mbedtls_sha256_init(&ctx->accumulator);
#endif
#if defined(MBEDTLS_HAVEGE_C)
	mbedtls_havege_init(&ctx->havege_data);
#endif

	/* Reminder: Update ENTROPY_HAVE_STRONG in the test files
	 *		   when adding more strong entropy sources here. */

#if !defined(MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES)
	mbedtls_entropy_add_source(ctx, mbedtls_hardclock_poll, NULL,
								MBEDTLS_ENTROPY_MIN_HARDCLOCK,
								MBEDTLS_ENTROPY_SOURCE_WEAK);
#if defined(MBEDTLS_HAVEGE_C)
	mbedtls_entropy_add_source(ctx, mbedtls_havege_poll, &ctx->havege_data,
								MBEDTLS_ENTROPY_MIN_HAVEGE,
								MBEDTLS_ENTROPY_SOURCE_STRONG);
#endif
	mbedtls_entropy_add_source(ctx, mbedtls_hardware_poll, NULL,
								MBEDTLS_ENTROPY_MIN_HARDWARE,
								MBEDTLS_ENTROPY_SOURCE_STRONG);
#endif /* MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES */
}

void mbedtls_entropy_free(mbedtls_entropy_context *ctx)
{
#if defined(MBEDTLS_HAVEGE_C)
	mbedtls_havege_free(&ctx->havege_data);
#endif
#if defined(MBEDTLS_ENTROPY_SHA512_ACCUMULATOR)
	mbedtls_sha512_free(&ctx->accumulator);
#else
	mbedtls_sha256_free(&ctx->accumulator);
#endif
	ctx->source_count = 0;
	mbedtls_zeroize(ctx->source, sizeof(ctx->source));
	ctx->accumulator_started = 0;
}

int mbedtls_entropy_add_source(mbedtls_entropy_context *ctx,
						mbedtls_entropy_f_source_ptr f_source, void *p_source,
						size_t threshold, int strong)
{
	int idx, ret = 0;

	spin_lock(&ctx->mutex);

	idx = ctx->source_count;
	if (idx >= MBEDTLS_ENTROPY_MAX_SOURCES)
	{
		ret = MBEDTLS_ERR_ENTROPY_MAX_SOURCES;
		goto exit;
	}

	ctx->source[idx].f_source  = f_source;
	ctx->source[idx].p_source  = p_source;
	ctx->source[idx].threshold = threshold;
	ctx->source[idx].strong	= strong;

	ctx->source_count++;

exit:
	spin_unlock(&ctx->mutex);

	return ret;
}

/*
 * Entropy accumulator update
 */
static int entropy_update(mbedtls_entropy_context *ctx, unsigned char source_id,
						   const unsigned char *data, size_t len)
{
	unsigned char header[2];
	unsigned char tmp[MBEDTLS_ENTROPY_BLOCK_SIZE];
	size_t use_len = len;
	const unsigned char *p = data;
	int ret = 0;

	if (use_len > MBEDTLS_ENTROPY_BLOCK_SIZE)
	{
#if defined(MBEDTLS_ENTROPY_SHA512_ACCUMULATOR)
		if ((ret = mbedtls_sha512_ret(data, len, tmp, 0)) != 0)
			goto cleanup;
#else
		if ((ret = mbedtls_sha256_ret(data, len, tmp, 0)) != 0)
			goto cleanup;
#endif
		p = tmp;
		use_len = MBEDTLS_ENTROPY_BLOCK_SIZE;
	}

	header[0] = source_id;
	header[1] = use_len & 0xFF;

	/*
	 * Start the accumulator if this has not already happened. Note that
	 * it is sufficient to start the accumulator here only because all calls to
	 * gather entropy eventually execute this code.
	 */
#if defined(MBEDTLS_ENTROPY_SHA512_ACCUMULATOR)
	if (ctx->accumulator_started == 0 &&
		(ret = mbedtls_sha512_starts_ret(&ctx->accumulator, 0)) != 0)
		goto cleanup;
	else
		ctx->accumulator_started = 1;
	if ((ret = mbedtls_sha512_update_ret(&ctx->accumulator, header, 2)) != 0)
		goto cleanup;
	ret = mbedtls_sha512_update_ret(&ctx->accumulator, p, use_len);
#else
	if (ctx->accumulator_started == 0 &&
		(ret = mbedtls_sha256_starts_ret(&ctx->accumulator, 0)) != 0)
		goto cleanup;
	else
		ctx->accumulator_started = 1;
	if ((ret = mbedtls_sha256_update_ret(&ctx->accumulator, header, 2)) != 0)
		goto cleanup;
	ret = mbedtls_sha256_update_ret(&ctx->accumulator, p, use_len);
#endif

cleanup:
	mbedtls_zeroize(tmp, sizeof(tmp));

	return ret;
}

int mbedtls_entropy_update_manual(mbedtls_entropy_context *ctx,
						   const unsigned char *data, size_t len)
{
	int ret;

	spin_lock(&ctx->mutex);

	ret = entropy_update(ctx, MBEDTLS_ENTROPY_SOURCE_MANUAL, data, len);

	spin_unlock(&ctx->mutex);

	return ret;
}

/*
 * Run through the different sources to add entropy to our accumulator
 */
static int entropy_gather_internal(mbedtls_entropy_context *ctx)
{
	int ret, i, have_one_strong = 0;
	unsigned char buf[MBEDTLS_ENTROPY_MAX_GATHER];
	size_t olen;

	if (ctx->source_count == 0)
		return(MBEDTLS_ERR_ENTROPY_NO_SOURCES_DEFINED);

	/*
	 * Run through our entropy sources
	 */
	for (i = 0; i < ctx->source_count; i++)
	{
		if (ctx->source[i].strong == MBEDTLS_ENTROPY_SOURCE_STRONG)
			have_one_strong = 1;

		olen = 0;
		if ((ret = ctx->source[i].f_source(ctx->source[i].p_source,
						buf, MBEDTLS_ENTROPY_MAX_GATHER, &olen)) != 0)
		{
			goto cleanup;
		}

		/*
		 * Add if we actually gathered something
		 */
		if (olen > 0)
		{
			if ((ret = entropy_update(ctx, (unsigned char) i,
										buf, olen)) != 0)
				return ret;
			ctx->source[i].size += olen;
		}
	}

	if (have_one_strong == 0)
		ret = MBEDTLS_ERR_ENTROPY_NO_STRONG_SOURCE;

cleanup:
	mbedtls_zeroize(buf, sizeof(buf));

	return ret;
}

/*
 * Thread-safe wrapper for entropy_gather_internal()
 */
int mbedtls_entropy_gather(mbedtls_entropy_context *ctx)
{
	int ret;

	spin_lock(&ctx->mutex);

	ret = entropy_gather_internal(ctx);

	spin_unlock(&ctx->mutex);

	return ret;
}

int mbedtls_entropy_func(void *data, unsigned char *output, size_t len)
{
	int ret, count = 0, i, done;
	mbedtls_entropy_context *ctx = (mbedtls_entropy_context *) data;
	unsigned char buf[MBEDTLS_ENTROPY_BLOCK_SIZE];

	if (len > MBEDTLS_ENTROPY_BLOCK_SIZE)
		return(MBEDTLS_ERR_ENTROPY_SOURCE_FAILED);

	spin_lock(&ctx->mutex);

	/*
	 * Always gather extra entropy before a call
	 */
	do
	{
		if (count++ > ENTROPY_MAX_LOOP)
		{
			ret = MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
			goto exit;
		}

		if ((ret = entropy_gather_internal(ctx)) != 0)
			goto exit;

		done = 1;
		for (i = 0; i < ctx->source_count; i++)
			if (ctx->source[i].size < ctx->source[i].threshold)
				done = 0;
	}
	while (! done);

	memset(buf, 0, MBEDTLS_ENTROPY_BLOCK_SIZE);

#if defined(MBEDTLS_ENTROPY_SHA512_ACCUMULATOR)
	/*
	 * Note that at this stage it is assumed that the accumulator was started
	 * in a previous call to entropy_update(). If this is not guaranteed, the
	 * code below will fail.
	 */
	if ((ret = mbedtls_sha512_finish_ret(&ctx->accumulator, buf)) != 0)
		goto exit;

	/*
	 * Reset accumulator and counters and recycle existing entropy
	 */
	mbedtls_sha512_free(&ctx->accumulator);
	mbedtls_sha512_init(&ctx->accumulator);
	if ((ret = mbedtls_sha512_starts_ret(&ctx->accumulator, 0)) != 0)
		goto exit;
	if ((ret = mbedtls_sha512_update_ret(&ctx->accumulator, buf,
										   MBEDTLS_ENTROPY_BLOCK_SIZE)) != 0)
		goto exit;

	/*
	 * Perform second SHA-512 on entropy
	 */
	if ((ret = mbedtls_sha512_ret(buf, MBEDTLS_ENTROPY_BLOCK_SIZE,
									buf, 0)) != 0)
		goto exit;
#else /* MBEDTLS_ENTROPY_SHA512_ACCUMULATOR */
	if ((ret = mbedtls_sha256_finish_ret(&ctx->accumulator, buf)) != 0)
		goto exit;

	/*
	 * Reset accumulator and counters and recycle existing entropy
	 */
	mbedtls_sha256_free(&ctx->accumulator);
	mbedtls_sha256_init(&ctx->accumulator);
	if ((ret = mbedtls_sha256_starts_ret(&ctx->accumulator, 0)) != 0)
		goto exit;
	if ((ret = mbedtls_sha256_update_ret(&ctx->accumulator, buf,
										   MBEDTLS_ENTROPY_BLOCK_SIZE)) != 0)
		goto exit;

	/*
	 * Perform second SHA-256 on entropy
	 */
	if ((ret = mbedtls_sha256_ret(buf, MBEDTLS_ENTROPY_BLOCK_SIZE,
									buf, 0)) != 0)
		goto exit;
#endif /* MBEDTLS_ENTROPY_SHA512_ACCUMULATOR */

	for (i = 0; i < ctx->source_count; i++)
		ctx->source[i].size = 0;

	memcpy(output, buf, len);

	ret = 0;

exit:
	mbedtls_zeroize(buf, sizeof(buf));

	spin_unlock(&ctx->mutex);

	return ret;
}

/*
 * Dummy source function
 */
static int entropy_dummy_source(void *data, unsigned char *output,
								 size_t len, size_t *olen)
{
	((void) data);

	memset(output, 0x2a, len);
	*olen = len;

	return 0;
}

static int mbedtls_entropy_source_self_test_gather(unsigned char *buf, size_t buf_len)
{
	int ret = 0;
	size_t entropy_len = 0;
	size_t olen = 0;
	size_t attempts = buf_len;

	while (attempts > 0 && entropy_len < buf_len)
	{
		if ((ret = mbedtls_hardware_poll(NULL, buf + entropy_len,
			buf_len - entropy_len, &olen)) != 0)
			return ret;

		entropy_len += olen;
		attempts--;
	}

	if (entropy_len < buf_len)
	{
		ret = 1;
	}

	return ret;
}


static int mbedtls_entropy_source_self_test_check_bits(const unsigned char *buf,
														size_t buf_len)
{
	unsigned char set= 0xFF;
	unsigned char unset = 0x00;
	size_t i;

	for (i = 0; i < buf_len; i++)
	{
		set &= buf[i];
		unset |= buf[i];
	}

	return(set == 0xFF || unset == 0x00);
}

/*
 * A test to ensure hat the entropy sources are functioning correctly
 * and there is no obvious failure. The test performs the following checks:
 *  - The entropy source is not providing only 0s (all bits unset) or 1s (all
 *	bits set).
 *  - The entropy source is not providing values in a pattern. Because the
 *	hardware could be providing data in an arbitrary length, this check polls
 *	the hardware entropy source twice and compares the result to ensure they
 *	are not equal.
 *  - The error code returned by the entropy source is not an error.
 */
int mbedtls_entropy_source_self_test(int verbose)
{
	int ret = 0;
	unsigned char buf0[2 * sizeof(unsigned long long int)];
	unsigned char buf1[2 * sizeof(unsigned long long int)];

	if (verbose != 0)
		mbedtls_printf("  ENTROPY_BIAS test: ");

	memset(buf0, 0x00, sizeof(buf0));
	memset(buf1, 0x00, sizeof(buf1));

	if ((ret = mbedtls_entropy_source_self_test_gather(buf0, sizeof(buf0))) != 0)
		goto cleanup;
	if ((ret = mbedtls_entropy_source_self_test_gather(buf1, sizeof(buf1))) != 0)
		goto cleanup;

	/* Make sure that the returned values are not all 0 or 1 */
	if ((ret = mbedtls_entropy_source_self_test_check_bits(buf0, sizeof(buf0))) != 0)
		goto cleanup;
	if ((ret = mbedtls_entropy_source_self_test_check_bits(buf1, sizeof(buf1))) != 0)
		goto cleanup;

	/* Make sure that the entropy source is not returning values in a
	 * pattern */
	ret = memcmp(buf0, buf1, sizeof(buf0)) == 0;

cleanup:
	if (verbose != 0)
	{
		if (ret != 0)
			mbedtls_printf("failed\n");
		else
			mbedtls_printf("passed\n");

		mbedtls_printf("\n");
	}

	return(ret != 0);
}

/*
 * The actual entropy quality is hard to test, but we can at least
 * test that the functions don't cause errors and write the correct
 * amount of data to buffers.
 */
int mbedtls_entropy_self_test(int verbose)
{
	int ret = 1;
	mbedtls_entropy_context ctx;
	unsigned char buf[MBEDTLS_ENTROPY_BLOCK_SIZE] = { 0 };
	unsigned char acc[MBEDTLS_ENTROPY_BLOCK_SIZE] = { 0 };
	size_t i, j;

	if (verbose != 0)
		mbedtls_printf("  ENTROPY test: ");

	mbedtls_entropy_init(&ctx);

	/* First do a gather to make sure we have default sources */
	if ((ret = mbedtls_entropy_gather(&ctx)) != 0)
		goto cleanup;

	ret = mbedtls_entropy_add_source(&ctx, entropy_dummy_source, NULL, 16,
									  MBEDTLS_ENTROPY_SOURCE_WEAK);
	if (ret != 0)
		goto cleanup;

	if ((ret = mbedtls_entropy_update_manual(&ctx, buf, sizeof buf)) != 0)
		goto cleanup;

	/*
	 * To test that mbedtls_entropy_func writes correct number of bytes:
	 * - use the whole buffer and rely on ASan to detect overruns
	 * - collect entropy 8 times and OR the result in an accumulator:
	 *   any byte should then be 0 with probably 2^(-64), so requiring
	 *   each of the 32 or 64 bytes to be non-zero has a false failure rate
	 *   of at most 2^(-58) which is acceptable.
	 */
	for (i = 0; i < 8; i++)
	{
		if ((ret = mbedtls_entropy_func(&ctx, buf, sizeof(buf))) != 0)
			goto cleanup;

		for (j = 0; j < sizeof(buf); j++)
			acc[j] |= buf[j];
	}

	for (j = 0; j < sizeof(buf); j++)
	{
		if (acc[j] == 0)
		{
			ret = 1;
			goto cleanup;
		}
	}

	if ((ret = mbedtls_entropy_source_self_test(0)) != 0)
		goto cleanup;

cleanup:
	mbedtls_entropy_free(&ctx);

	if (verbose != 0)
	{
		if (ret != 0)
			mbedtls_printf("failed\n");
		else
			mbedtls_printf("passed\n");

		mbedtls_printf("\n");
	}

	return(ret != 0);
}

#endif /* MBEDTLS_ENTROPY_C */
