/*-
 * Copyright (c) 2009-2012 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/param.h>

#include <xbps_api.h>
#include "defs.h"

struct checkpkg {
	size_t npkgs;
	size_t nbrokenpkgs;
	bool flush;
};

static int
cb_pkg_integrity(prop_object_t obj, void *arg, bool *done)
{
	struct checkpkg *cpkg = arg;
	const char *pkgname, *version;
	bool flush = false;

	(void)done;

	prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
	prop_dictionary_get_cstring_nocopy(obj, "version", &version);
	printf("Checking %s-%s ...\n", pkgname, version);
	if (check_pkg_integrity(obj, pkgname, false, &flush) != 0)
		cpkg->nbrokenpkgs++;
	else
		printf("\033[1A\033[K");

	if (flush && !cpkg->flush)
		cpkg->flush = flush;
	cpkg->npkgs++;
	return 0;
}

int
check_pkg_integrity_all(void)
{
	struct checkpkg cpkg;
	int rv;

	memset(&cpkg, 0, sizeof(cpkg));
	(void)xbps_pkgdb_foreach_cb(cb_pkg_integrity, &cpkg);
	if (cpkg.flush) {
		if ((rv = xbps_pkgdb_update(true)) != 0) {
			xbps_error_printf("failed to write pkgdb: %s\n",
			    strerror(rv));
			return rv;
		}
	}
	printf("%zu package%s processed: %zu broken.\n", cpkg.npkgs,
	    cpkg.npkgs == 1 ? "" : "s", cpkg.nbrokenpkgs);
	return 0;
}

int
check_pkg_integrity(prop_dictionary_t pkgd,
		    const char *pkgname,
		    bool flush,
		    bool *setflush)
{
	prop_dictionary_t opkgd, propsd, filesd;
	int rv = 0;
	bool pkgdb_update = false, broken = false;

	propsd = filesd = opkgd = NULL;

	/* find real pkg by name */
	if (pkgd == NULL) {
		opkgd = xbps_find_pkg_dict_installed(pkgname, false);
		if (opkgd == NULL) {
			/* find virtual pkg by name */
			opkgd = xbps_find_virtualpkg_dict_installed(pkgname,
			    false);
		}
		if (opkgd == NULL) {
			printf("Package %s is not installed.\n", pkgname);
			return 0;
		}
	}
	/*
	 * Check for props.plist metadata file.
	 */
	propsd = xbps_dictionary_from_metadata_plist(pkgname, XBPS_PKGPROPS);
	if (propsd == NULL) {
		xbps_error_printf("%s: unexistent %s or invalid metadata "
		    "file.\n", pkgname, XBPS_PKGPROPS);
		broken = true;
		goto out;
	} else if (prop_dictionary_count(propsd) == 0) {
		xbps_error_printf("%s: incomplete %s metadata file.\n",
		    pkgname, XBPS_PKGPROPS);
		broken = true;
		goto out;
	}
	/*
	 * Check for files.plist metadata file.
	 */
	filesd = xbps_dictionary_from_metadata_plist(pkgname, XBPS_PKGFILES);
	if (filesd == NULL) {
		xbps_error_printf("%s: unexistent %s or invalid metadata "
		    "file.\n", pkgname, XBPS_PKGFILES);
		broken = true;
		goto out;
	} else if (prop_dictionary_count(filesd) == 0) {
		xbps_error_printf("%s: incomplete %s metadata file.\n",
		    pkgname, XBPS_PKGFILES);
		broken = true;
		goto out;
	}

#define RUN_PKG_CHECK(name, arg, arg2)				\
do {								\
	rv = check_pkg_##name(pkgname, arg, arg2);		\
	if (rv)							\
		broken = true;					\
	else if (rv == -1) {					\
		xbps_error_printf("%s: the %s test "		\
		    "returned error!\n", pkgname, #name);	\
		goto out;					\
	}							\
} while (0)

	/* Execute pkg checks */
	RUN_PKG_CHECK(files, filesd, &pkgdb_update);
	RUN_PKG_CHECK(symlinks, filesd, &pkgdb_update);
	RUN_PKG_CHECK(rundeps, propsd, &pkgdb_update);
	RUN_PKG_CHECK(requiredby, pkgd ? pkgd : opkgd, &pkgdb_update);
	RUN_PKG_CHECK(autoinstall, pkgd ? pkgd : opkgd, &pkgdb_update);

	if (flush && pkgdb_update) {
		if (!xbps_pkgdb_replace_pkgd(opkgd, pkgname, false, true)) {
			rv = EINVAL;
			goto out;
		}
	}
	if (setflush && pkgdb_update)
		*setflush = true;

#undef RUN_PKG_CHECK

out:
	if (prop_object_type(filesd) == PROP_TYPE_DICTIONARY)
		prop_object_release(filesd);
	if (prop_object_type(propsd) == PROP_TYPE_DICTIONARY)
		prop_object_release(propsd);
	if (prop_object_type(opkgd) == PROP_TYPE_DICTIONARY)
		prop_object_release(opkgd);
	if (broken)
		return 1;

	return 0;
}
