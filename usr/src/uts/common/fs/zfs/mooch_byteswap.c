/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2013, 2015 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/mooch_byteswap.h>
#include <sys/blkptr.h>
#include <sys/zio.h>
#include <sys/zio_compress.h>

#include <sys/dbuf.h>
#include <sys/dnode.h>

/*
 * Mooch-Byteswap Block Pointers
 *
 * Mooch-Byteswap block pointers allow for a block to be stored very
 * compactly if it is a byteswapped version of another block.
 * Specifically, a file in a clone may be a byteswapped version of a
 * file in its origin snapshot.
 *
 * Mooched BP's are a type of embedded block pointer, see blkptr.c for
 * details.  On contrast with BP_EMBEDDED_TYPE_DATA, these
 * BP_EMBEDDED_TYPE_MOOCH_BYTESWAP block pointers are handled mainly in
 * the DMU; the SPA does not see them.
 *
 * The ZPL stores a mapping of which files are byteswapped versions of
 * files in the origin.  It maps from object ID in this filesytem (the
 * clone) to the object ID of the byteswapped file in the origin
 * snapshot.  This mapping is created by using the
 * _FIO_MOOCH_BYTESWAP_MAP ioctl.   Storing the relationship in the ZPL
 * allowed for this feature to be added without modifying the on-disk
 * structure of the dnode_phys_t.  The DMU caches the mapping in the
 * in-memory dnode_t.
 *
 * When a block is written, the DMU will notice that it is part of a
 * "mooching" object.  If this is the first time the block is written,
 * it will read the block at the same offset in the mooched file (from
 * the origin snapshot -- the "old block"), and attempt to infer the
 * byteswap operation that the application applied.
 * mooch_byteswap_determine() will determine if the new block can be
 * reconstructed by applying a series of byteswap instructions to the
 * old block.  If so, these byteswap instructions will be stored
 * (compressed if necessary) as the payload of an embedded block
 * pointer, rather than writing the new data.
 *
 * When a block is read, the DMU will notice that it is an embedded
 * block pointer of type BP_EMBEDDED_TYPE_MOOCH_BYTESWAP.  It will read
 * the old block (the block at the same offset in the mooched file in
 * the origin) and then apply the byteswap instructions from the
 * embedded block pointer's payload.
 */

static uint16_t
xor16(const void *buf, int len)
{
	const uint16_t *data = buf;
	uint16_t sum = 0;
	for (int i = 0; i < len / sizeof (*data); i++)
		sum ^= data[i];
	return (sum);
}

typedef struct byteswap_data {
	bsrec_t *bd_records;
	int bd_numrecords;
	bsrec_instr_t bd_lastinstr;
	int bd_lastcount;
	boolean_t bd_have_xor;

	const char *bd_oldbuf;
	const char *bd_newbuf;
	int bd_buflen;
	int bd_dataoff;
} byteswap_data_t;

static void
flushlast(byteswap_data_t *bd)
{
	while (bd->bd_lastcount > 0) {
		int count = MIN(BSREC_MAX_DATA, bd->bd_lastcount);
		bsrec_t newrec = 0;
		if (bd->bd_lastinstr == BSREC_INSTR_SKIP8) {
			BSREC_SET_TYPE(newrec, BSREC_TYPE_SKIP);
			BSREC_SET_DATA(newrec, count);
		} else {
			if (count > 1) {
				BSREC_SET_TYPE(newrec, BSREC_TYPE_REPEAT);
				BSREC_SET_DATA(newrec, count);
				bd->bd_records[bd->bd_numrecords++] = newrec;
				newrec = 0;
			}
			BSREC_SET_TYPE(newrec, BSREC_TYPE_INSTR);
			BSREC_SET_DATA(newrec, bd->bd_lastinstr);
		}
		bd->bd_records[bd->bd_numrecords++] = newrec;
		bd->bd_lastcount -= count;
	}
}

static void
newinstr(byteswap_data_t *bd, bsrec_instr_t instr)
{
	if (instr == bd->bd_lastinstr) {
		bd->bd_lastcount++;
	} else {
		flushlast(bd);
		bd->bd_lastinstr = instr;
		bd->bd_lastcount = 1;
	}
	switch (instr) {
	case BSREC_INSTR_SWAP64:
		bd->bd_dataoff += sizeof (uint64_t);
		break;
	case BSREC_INSTR_SWAP48:
		bd->bd_dataoff += 6;
		break;
	case BSREC_INSTR_SWAP32:
		bd->bd_dataoff += sizeof (uint32_t);
		break;
	case BSREC_INSTR_SWAP16:
	case BSREC_INSTR_XOR16:
		bd->bd_dataoff += sizeof (uint16_t);
		break;
	case BSREC_INSTR_SKIP8:
		bd->bd_dataoff += sizeof (uint8_t);
		break;
	default:
		panic("invalid instruction type %u", (int)instr);
	}
}

static void
newskip(byteswap_data_t *bd, int runlen)
{
	if (bd->bd_lastinstr == BSREC_INSTR_SKIP8) {
		bd->bd_lastcount += runlen;
	} else {
		flushlast(bd);
		bd->bd_lastinstr = BSREC_INSTR_SKIP8;
		bd->bd_lastcount = runlen;
	}
	bd->bd_dataoff += runlen;
}

static int
checkswap(byteswap_data_t *bd)
{
	const int8_t *oldp = bd->bd_oldbuf + bd->bd_dataoff;
	const int8_t *newp = bd->bd_newbuf + bd->bd_dataoff;

	/*
	 * Fast path for equal bytes.
	 */
	int i;
	for (i = 0; bd->bd_dataoff + i < bd->bd_buflen; i++)
		if (oldp[i] != newp[i])
			break;
	if (i > 0) {
		newskip(bd, i);
		return (0);
	}

	/*
	 * Check for 64-bit word.
	 */
	if (bd->bd_dataoff + sizeof (uint64_t) <= bd->bd_buflen &&
	    oldp[0] == newp[7] &&
	    oldp[1] == newp[6] &&
	    oldp[2] == newp[5] &&
	    oldp[3] == newp[4] &&
	    oldp[4] == newp[3] &&
	    oldp[5] == newp[2] &&
	    oldp[6] == newp[1] &&
	    oldp[7] == newp[0]) {
		newinstr(bd, BSREC_INSTR_SWAP64);
		return (0);
	}

	/*
	 * Check for 6-byte "word".
	 *
	 * Note, an 8-byte word whose first and last byte are the
	 * same will appear to be a 6-byte word.  This is because we check for
	 * equal bytes first.  If the first and last bytes of an 8-byte
	 * word are the same, then we will interpret its first byte as
	 * being a "same byte" and thus not needing swapping.
	 *
	 * Another way to address this would be to check for an 8-byte word
	 * before checking for any same bytes, but that would negatively
	 * impact performance.
	 */
	if (bd->bd_dataoff + 6 <= bd->bd_buflen &&
	    oldp[0] == newp[5] &&
	    oldp[1] == newp[4] &&
	    oldp[2] == newp[3] &&
	    oldp[3] == newp[2] &&
	    oldp[4] == newp[1] &&
	    oldp[5] == newp[0]) {
		newinstr(bd, BSREC_INSTR_SWAP48);
		return (0);
	}

	/* check for 32-bit word */
	if (bd->bd_dataoff + sizeof (uint32_t) <= bd->bd_buflen &&
	    oldp[0] == newp[3] &&
	    oldp[1] == newp[2] &&
	    oldp[2] == newp[1] &&
	    oldp[3] == newp[0]) {
		newinstr(bd, BSREC_INSTR_SWAP32);
		return (0);
	}

	/* check for 16-bit word */
	if (bd->bd_dataoff + sizeof (uint16_t) <= bd->bd_buflen &&
	    oldp[0] == newp[1] &&
	    oldp[1] == newp[0]) {
		newinstr(bd, BSREC_INSTR_SWAP16);
		return (0);
	}

	/*
	 * If the new block 16-bit xor is zero, we can recompute one 16-bit word
	 * with this parity information.  Typically this would happen if the
	 * block contains a 16-bit xor checksum.  The checksum can be
	 * reconstructed by the XOR16 instruction.
	 */
	if (!bd->bd_have_xor &&
	    IS_P2ALIGNED(bd->bd_dataoff, sizeof (uint16_t)) &&
	    xor16(bd->bd_newbuf, bd->bd_buflen) == 0) {
		newinstr(bd, BSREC_INSTR_XOR16);
		bd->bd_have_xor = B_TRUE;
		return (0);
	}

	return (EINVAL);
}

#define	BYTESWAP_HISTO_SIZE 2048
static uint64_t byteswap_blocks_with_n_recs[BYTESWAP_HISTO_SIZE];
static uint64_t byteswap_blocks_with_n_bytes[BYTESWAP_HISTO_SIZE];
static uint64_t byteswap_blocks_didnt_fit;
static uint64_t byteswap_blocks_untranslatable;
static boolean_t byteswap_reset_stats; /* set to 1 to reset stats */

/*
 * Determine if newbuf is a byteswapped version of oldbuf.  If so, fill in
 * the bp and return 0.  Otherwise, return nonzero.
 */
int
mooch_byteswap_determine(dmu_buf_t *oldbuf, dmu_buf_t *newbuf, blkptr_t *bp)
{
	byteswap_data_t bd = { 0 };
	int error;
	dmu_object_type_t type;

	dmu_buf_impl_t *newdbi = (dmu_buf_impl_t *)newbuf;
	DB_DNODE_ENTER(newdbi);
	ASSERT3U(DB_DNODE(newdbi)->dn_origin_obj_refd, ==, oldbuf->db_object);
	type = DB_DNODE(newdbi)->dn_type;
	DB_DNODE_EXIT(newdbi);

	if (oldbuf->db_size != newbuf->db_size)
		return (SET_ERROR(EINVAL));

	if (byteswap_reset_stats) {
		bzero(byteswap_blocks_with_n_recs,
		    sizeof (byteswap_blocks_with_n_recs));
		bzero(byteswap_blocks_with_n_bytes,
		    sizeof (byteswap_blocks_with_n_bytes));
		byteswap_blocks_didnt_fit = 0;
		byteswap_blocks_untranslatable = 0;
		byteswap_reset_stats = B_FALSE;
	}

	bd.bd_records = kmem_alloc(oldbuf->db_size, KM_SLEEP);
	bd.bd_oldbuf = abd_borrow_buf_copy(oldbuf->db_data, oldbuf->db_size);
	bd.bd_newbuf = abd_borrow_buf_copy(newbuf->db_data, newbuf->db_size);
	bd.bd_buflen = oldbuf->db_size;

	while (bd.bd_dataoff < oldbuf->db_size) {
		error = checkswap(&bd);
		if (error != 0)
			break;
	}
	flushlast(&bd);
	if (error == 0) {
		error = encode_embedded_bp(bp, bd.bd_records,
		    bd.bd_numrecords * sizeof (*bd.bd_records));

		atomic_add_64(&byteswap_blocks_with_n_recs
		    [MIN(BYTESWAP_HISTO_SIZE - 1, bd.bd_numrecords)], 1);

		if (error == 0) {
			BPE_SET_ETYPE(bp, BP_EMBEDDED_TYPE_MOOCH_BYTESWAP);
			BP_SET_TYPE(bp, type);
			BP_SET_LEVEL(bp, 0);

			atomic_add_64(&byteswap_blocks_with_n_bytes
			    [MIN(BYTESWAP_HISTO_SIZE - 1, BPE_GET_PSIZE(bp))],
			    1);
		} else {
			atomic_add_64(&byteswap_blocks_didnt_fit, 1);
		}
	} else {
		atomic_add_64(&byteswap_blocks_untranslatable, 1);
	}

	abd_return_buf(oldbuf->db_data, (void *)bd.bd_oldbuf, oldbuf->db_size);
	abd_return_buf(newbuf->db_data, (void *)bd.bd_newbuf, newbuf->db_size);
	kmem_free(bd.bd_records, oldbuf->db_size);
	return (error);
}

static void
doinstr(bsrec_instr_t instr, uint8_t *data, int *offp)
{
#define	SWAP(x, y) do { \
	uint8_t tmp = data[off + x]; \
	data[off + x] = data[off + y]; \
	data[off + y] = tmp; \
_NOTE(CONSTCOND) } while (0)

	int off = *offp;
	switch (instr) {
	case BSREC_INSTR_SWAP64:
		if (IS_P2ALIGNED(off, sizeof (uint64_t))) {
			uint64_t *p = (uint64_t *)(data + off);
			*p = BSWAP_64(*p);
		} else {
			SWAP(0, 7);
			SWAP(1, 6);
			SWAP(2, 5);
			SWAP(3, 4);
		}
		*offp += sizeof (uint64_t);
		break;
	case BSREC_INSTR_SWAP48:
		SWAP(0, 5);
		SWAP(1, 4);
		SWAP(2, 3);
		*offp += 6;
		break;
	case BSREC_INSTR_SWAP32:
		if (IS_P2ALIGNED(off, sizeof (uint32_t))) {
			uint32_t *p = (uint32_t *)(data + off);
			*p = BSWAP_32(*p);
		} else {
			SWAP(0, 3);
			SWAP(1, 2);
		}
		*offp += sizeof (uint32_t);
		break;
	case BSREC_INSTR_SWAP16:
		SWAP(0, 1);
		*offp += sizeof (uint16_t);
		break;
	case BSREC_INSTR_SKIP8:
		*offp += sizeof (uint8_t);
		break;
	default:
		panic("invalid byteswap instruction %u", (int)instr);
	}
#undef SWAP
}

/*
 * Fill in outbuf with the reconstructed data based on old and the
 * (NON_POINTER) bp.
 */
void
mooch_byteswap_reconstruct(dmu_buf_t *old, void *outbuf, const blkptr_t *bp)
{
	bsrec_t *records;
	int nrec;
	int offset, repeat;
	int xor_offset = -1;

	ASSERT(BP_IS_EMBEDDED(bp));
	VERIFY3P(old->db_data, !=, NULL);
	VERIFY3P(outbuf, !=, NULL);

	nrec = BPE_GET_LSIZE(bp);
	records = kmem_alloc(nrec, KM_SLEEP);

	VERIFY0(decode_embedded_bp(bp, records, nrec));

	bcopy(old->db_data, outbuf, old->db_size);

	offset = 0;
	repeat = 1;
	for (int i = 0; i < nrec; i++) {
		switch (BSREC_GET_TYPE(records[i])) {
		case BSREC_TYPE_SKIP:
			offset += BSREC_GET_DATA(records[i]);
			break;
		case BSREC_TYPE_REPEAT:
			repeat = BSREC_GET_DATA(records[i]);
			break;
		case BSREC_TYPE_INSTR: {
			bsrec_instr_t instr = BSREC_GET_DATA(records[i]);
			if (instr == BSREC_INSTR_XOR16) {
				/*
				 * There can only be one XOR16 instruction
				 * per block.  Remember its offset and process
				 * after all other instructions.
				 */
				ASSERT3S(xor_offset, ==, -1);
				ASSERT3U(repeat, ==, 1);
				ASSERT(IS_P2ALIGNED(offset, sizeof (uint16_t)));

				xor_offset = offset;
				offset += sizeof (uint16_t);
				break;
			}
			ASSERT3U(repeat, >=, 1);
			for (int j = 0; j < repeat; j++)
				doinstr(instr, outbuf, &offset);
			repeat = 1;
			break;
		}
		default:
			panic("invalid bsrec type for record %x", records[i]);
		}
	}

	if (xor_offset != -1) {
		/*
		 * The XOR16 instruction indicates that this 16-bit word is
		 * the 16-bit xor of all other words in the block.
		 */
		uint16_t *xorp = (uint16_t *)((char *)outbuf + xor_offset);
		*xorp = 0;
		*xorp = xor16(outbuf, old->db_size);
	}
	kmem_free(records, nrec);
}
