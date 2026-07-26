/*
 * Copyright (c) 2022 Josh Rickmar <jrick@zettaport.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/time.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "got_date.h"

void
got_date_format_gmtoff(char *buf, size_t sz, time_t gmtoff)
{
	long long h, m;
	char sign = '+';

	if (gmtoff < 0) {
		sign = '-';
		gmtoff = -gmtoff;
	}

	h = (long long)gmtoff / 3600;
	m = ((long long)gmtoff - h*3600) / 60;
	snprintf(buf, sz, "%c%02lld%02lld", sign, h, m);
}

struct tm *
got_date_get_tm(const time_t *t, struct tm *tm)
{
	static int tz_checked, have_tz;

	if (!tz_checked) {
		const char *tz = getenv("GOT_TZ");

		if (tz != NULL && tz[0] != '\0' && setenv("TZ", tz, 1) == 0) {
			tzset();
			have_tz = 1;
		}
		tz_checked = 1;
	}

	return have_tz ? localtime_r(t, tm) : gmtime_r(t, tm);
}

char *
got_date_get_zoneabbrev(char *buf, size_t sz, time_t t)
{
	struct tm tm;

	if (got_date_get_tm(&t, &tm) == NULL)
		return NULL;
	if (strftime(buf, sz, "%Z", &tm) == 0)
		return NULL;

	return buf;
}
