/*
 * Copyright (c) 2020 Yubico AB. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "fido.h"
#include "../openbsd-compat/openbsd-compat.h"

#define FIDO2_POLL_TIMEOUT	50	/* milliseconds */
#define U2F_POLL_TIMEOUT	300	/* milliseconds */

static fido_dev_t *
open_dev(const fido_dev_info_t *di)
{
	fido_dev_t	*dev;
	int		 r;

	if ((dev = fido_dev_new()) == NULL) {
		warnx("%s: fido_dev_new", __func__);
		return (NULL);
	}

	if ((r = fido_dev_open(dev, fido_dev_info_path(di))) != FIDO_OK) {
		warnx("%s: fido_dev_open %s: %s", __func__,
		    fido_dev_info_path(di), fido_strerr(r));
		fido_dev_free(&dev);
		return (NULL);
	}

	printf("%s (0x%04x:0x%04x) is %s\n", fido_dev_info_path(di),
	    fido_dev_info_vendor(di), fido_dev_info_product(di),
	    fido_dev_is_fido2(dev) ? "fido2" : "u2f");

	return (dev);
}

static int
select_dev(const fido_dev_info_t *devlist, size_t ndevs, fido_dev_t **dev,
    size_t *idx, int secs)
{
	const fido_dev_info_t	 *di;
	fido_dev_t		**devtab;
	struct timespec		  ts_start;
	struct timespec		  ts_now;
	struct timespec		  ts_delta;
	size_t			  nopen = 0;
	int			  touched;
	int			  r;
	int			  ms;
	long			  ms_remain;

	*dev = NULL;
	*idx = 0;

	printf("%u authenticator(s) detected\n", (unsigned)ndevs);

	if (ndevs == 0)
		return (0); /* nothing to do */

	if ((devtab = calloc(ndevs, sizeof(*devtab))) == NULL) {
		warn("%s: calloc", __func__);
		return (-1);
	}

	for (size_t i = 0; i < ndevs; i++) {
		di = fido_dev_info_ptr(devlist, i);
		if ((devtab[i] = open_dev(di)) != NULL) {
			*idx = i;
			nopen++;
		}
	}

	printf("%u authenticator(s) opened\n", (unsigned)nopen);

	if (nopen < 2) {
		if (nopen == 1)
			*dev = devtab[*idx]; /* single candidate */
		r = 0;
		goto out;
	}

	for (size_t i = 0; i < ndevs; i++) {
		di = fido_dev_info_ptr(devlist, i);
		if (devtab[i] == NULL)
			continue; /* failed to open */
		if ((r = fido_dev_get_touch_begin(devtab[i])) != FIDO_OK) {
			warnx("%s: fido_dev_get_touch_begin %s: %s", __func__,
			    fido_dev_info_path(di), fido_strerr(r));
			r = -1;
			goto out;
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &ts_start) != 0) {
		warn("%s: clock_gettime", __func__);
		r = -1;
		goto out;
	}

	do {
		for (size_t i = 0; i < ndevs; i++) {
			di = fido_dev_info_ptr(devlist, i);
			if (devtab[i] == NULL) {
				/* failed to open or discarded */
				continue;
			}
			if (fido_dev_is_fido2(devtab[i]))
				ms = FIDO2_POLL_TIMEOUT;
			else
				ms = U2F_POLL_TIMEOUT;
			if ((r = fido_dev_get_touch_status(devtab[i], &touched,
			    ms)) != FIDO_OK) {
				warnx("%s: fido_dev_get_touch_status %s: %s",
				    __func__, fido_dev_info_path(di),
				    fido_strerr(r));
				fido_dev_close(devtab[i]);
				fido_dev_free(&devtab[i]);
				continue; /* discard */
			}
			if (touched) {
				*dev = devtab[i];
				*idx = i;
				r = 0;
				goto out;
			}
		}

		if (clock_gettime(CLOCK_MONOTONIC, &ts_now) != 0) {
			warn("%s: clock_gettime", __func__);
			r = -1;
			goto out;
		}

		timespecsub(&ts_now, &ts_start, &ts_delta);
		ms_remain = (secs * 1000) - ((long)ts_delta.tv_sec * 1000) +
		    ((long)ts_delta.tv_nsec / 1000000);
	} while (ms_remain > U2F_POLL_TIMEOUT);

	printf("timeout after %d seconds\n", secs);
	r = -1;
out:
	if (r != 0) {
		*dev = NULL;
		*idx = 0;
	}

	for (size_t i = 0; i < ndevs; i++) {
		if (devtab[i] && devtab[i] != *dev) {
			fido_dev_cancel(devtab[i]);
			fido_dev_close(devtab[i]);
			fido_dev_free(&devtab[i]);
		}
	}

	free(devtab);

	return (r);
}

int
main(void)
{
	const fido_dev_info_t	*di;
	fido_dev_info_t		*devlist;
	fido_dev_t		*dev;
	size_t			 idx;
	size_t			 ndevs;
	int			 r;

	fido_init(0);

	if ((devlist = fido_dev_info_new(64)) == NULL)
		errx(1, "fido_dev_info_new");

	if ((r = fido_dev_info_manifest(devlist, 64, &ndevs)) != FIDO_OK)
		errx(1, "fido_dev_info_manifest: %s (0x%x)", fido_strerr(r), r);
	if (select_dev(devlist, ndevs, &dev, &idx, 15) != 0)
		errx(1, "select_dev");
	if (dev == NULL)
		errx(1, "no authenticator found");

	di = fido_dev_info_ptr(devlist, idx);
	printf("%s: %s by %s (PIN %sset)\n", fido_dev_info_path(di),
	    fido_dev_info_product_string(di),
	    fido_dev_info_manufacturer_string(di),
	    fido_dev_has_pin(dev) ? "" : "un");

	fido_dev_close(dev);
	fido_dev_free(&dev);
	fido_dev_info_free(&devlist, ndevs);

	exit(0);
}
