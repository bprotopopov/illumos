/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
#include <sys/zfs_context.h>
#include <sys/zio.h>
#include <sys/sha2.h>
#include <sys/abd.h>

static int
sha256_incremental(const void *buf, uint64_t size, void *arg)
{
	SHA2_CTX *ctx = arg;
	SHA2Update(ctx, buf, size);
	return (0);
}

void
abd_checksum_SHA256(abd_t *abd, uint64_t size, zio_cksum_t *zcp)
{
	SHA2_CTX ctx;
	zio_cksum_t tmp;

	SHA2Init(SHA256, &ctx);
	abd_iterate_rfunc(abd, size, sha256_incremental, &ctx);
	SHA2Final(&tmp, &ctx);

	/*
	 * A prior implementation of this function had a
	 * private SHA256 implementation always wrote things out in
	 * Big Endian and there wasn't a byteswap variant of it.
	 * To preserve on disk compatibility we need to force that
	 * behavior.
	 */
	zcp->zc_word[0] = BE_64(tmp.zc_word[0]);
	zcp->zc_word[1] = BE_64(tmp.zc_word[1]);
	zcp->zc_word[2] = BE_64(tmp.zc_word[2]);
	zcp->zc_word[3] = BE_64(tmp.zc_word[3]);
}

