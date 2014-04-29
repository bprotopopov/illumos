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
 * Copyright (c) 2014 by Delphix. All rights reserved.
 */

/*
 * ASSERTION:
 *   Verify that nested while loops work.
 */

#pragma D option experimental
#pragma D option quiet

BEGIN
{
	i = n = 0;
	while7 (i < 5) {
		j = 0;
		while5 (j < 3) {
			printf("i=%u j=%u n=%u\n", i, j, n);
			j++;
			n++;
		}
		i++;
	}
	exit(!(i == 5 && j == 3 && n == 5 * 3));
}
