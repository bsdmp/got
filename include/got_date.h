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

void
got_date_format_gmtoff(char *, size_t, time_t);

/*
 * Size of a buffer large enough to hold got_date_get_zoneabbrev()'s
 * result, including the terminating NUL. Not using the standard
 * TZNAME_MAX here: POSIX only guarantees _POSIX_TZNAME_MAX == 6,
 * which is too small for abbreviations such as "CEST", and TZNAME_MAX
 * itself is not portable (glibc does not define it at all).
 */
#define GOT_TZ_ABBREV_MAX	16

/*
 * Break a UTC time_t down into a struct tm for display. If the GOT_TZ
 * environment variable is set to a valid time zone name (as accepted by
 * tzset(3)), the time is converted to that zone; otherwise it remains
 * in UTC, matching got's historic default of always displaying dates
 * in UTC regardless of the committer's or the local system's time
 * zone. Same calling convention as gmtime_r(3)/localtime_r(3): returns
 * tm, or NULL on error.
 */
struct tm *got_date_get_tm(const time_t *, struct tm *);

/*
 * Obtain the abbreviated name of the time zone which got_date_get_tm()
 * would use to display t (e.g. "UTC", or the abbreviation of GOT_TZ's
 * zone at time t, accounting for that zone's daylight saving rules).
 * The result is written into buf (of size sz) and also returned, or
 * NULL on error.
 */
char *got_date_get_zoneabbrev(char *, size_t, time_t);
