/*
 * Copyright (C) 2008  Kristian Høgsberg <krh@redhat.com>
 * Copyright (C) 2008  Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmdb.h>

#include "razor.h"

union rpm_entry {
	void *p;
	char *string;
	char **list;
	uint_32 *flags;
	uint_32 integer;
};

static uint32_t
rpm_to_razor_flags(uint32_t flags)
{
	uint32_t razor_flags;

	razor_flags = 0;
	if (flags & RPMSENSE_LESS)
		razor_flags |= RAZOR_PROPERTY_LESS;
	if (flags & RPMSENSE_EQUAL)
		razor_flags |= RAZOR_PROPERTY_EQUAL;
	if (flags & RPMSENSE_GREATER)
		razor_flags |= RAZOR_PROPERTY_GREATER;

	if (flags & RPMSENSE_SCRIPT_PRE)
		razor_flags |= RAZOR_PROPERTY_PRE;
	if (flags & RPMSENSE_SCRIPT_POST)
		razor_flags |= RAZOR_PROPERTY_POST;
	if (flags & RPMSENSE_SCRIPT_PREUN)
		razor_flags |= RAZOR_PROPERTY_PREUN;
	if (flags & RPMSENSE_SCRIPT_POSTUN)
		razor_flags |= RAZOR_PROPERTY_POSTUN;

	return razor_flags;
}

static void
add_properties(struct razor_importer *importer,
	       uint32_t type_flags,
	       Header h, int_32 name_tag, int_32 version_tag, int_32 flags_tag)
{
	union rpm_entry names, versions, flags;
	int_32 i, type, count;

	headerGetEntry(h, name_tag, &type, &names.p, &count);
	headerGetEntry(h, version_tag, &type, &versions.p, &count);
	headerGetEntry(h, flags_tag, &type, &flags.p, &count);

	for (i = 0; i < count; i++)
		razor_importer_add_property(importer,
					    names.list[i],
					    rpm_to_razor_flags (flags.flags[i]) | type_flags,
					    versions.list[i]);
}

struct razor_set *
razor_set_create_from_rpmdb(void)
{
	struct razor_importer *importer;
	rpmdbMatchIterator iter;
	Header h;
	int_32 type, count, i;
	union rpm_entry name, epoch, version, release, arch;
	union rpm_entry summary, description, url, license;
	union rpm_entry basenames, dirnames, dirindexes;
	char filename[PATH_MAX], evr[128], buf[16];
	rpmdb db;
	int imported_count = 0;

	rpmReadConfigFiles(NULL, NULL);

	if (rpmdbOpen("", &db, O_RDONLY, 0644) != 0) {
		fprintf(stderr, "cannot open rpm database\n");
		exit(1);
	}

	importer = razor_importer_create();

	iter = rpmdbInitIterator(db, 0, NULL, 0);
	while (h = rpmdbNextIterator(iter), h != NULL) {
		headerGetEntry(h, RPMTAG_NAME, &type, &name.p, &count);
		headerGetEntry(h, RPMTAG_EPOCH, &type, &epoch.p, &count);
		headerGetEntry(h, RPMTAG_VERSION, &type, &version.p, &count);
		headerGetEntry(h, RPMTAG_RELEASE, &type, &release.p, &count);
		headerGetEntry(h, RPMTAG_ARCH, &type, &arch.p, &count);
		headerGetEntry(h, RPMTAG_SUMMARY, &type, &summary.p, &count);
		headerGetEntry(h, RPMTAG_DESCRIPTION, &type, &description.p,
			       &count);
		headerGetEntry(h, RPMTAG_URL, &type, &url.p, &count);
		headerGetEntry(h, RPMTAG_LICENSE, &type, &license.p, &count);

		if (epoch.flags != NULL) {
			snprintf(buf, sizeof buf, "%u", *epoch.flags);
			razor_build_evr(evr, sizeof evr,
					buf, version.string, release.string);
		} else {
			razor_build_evr(evr, sizeof evr,
					NULL, version.string, release.string);
		}

		razor_importer_begin_package(importer,
					     name.string, evr, arch.string);
		razor_importer_add_details(importer, summary.string,
					   description.string, url.string,
					   license.string);

		add_properties(importer, RAZOR_PROPERTY_REQUIRES, h,
			       RPMTAG_REQUIRENAME,
			       RPMTAG_REQUIREVERSION,
			       RPMTAG_REQUIREFLAGS);

		add_properties(importer, RAZOR_PROPERTY_PROVIDES, h,
			       RPMTAG_PROVIDENAME,
			       RPMTAG_PROVIDEVERSION,
			       RPMTAG_PROVIDEFLAGS);

		add_properties(importer, RAZOR_PROPERTY_OBSOLETES, h,
			       RPMTAG_OBSOLETENAME,
			       RPMTAG_OBSOLETEVERSION,
			       RPMTAG_OBSOLETEFLAGS);

		add_properties(importer, RAZOR_PROPERTY_CONFLICTS, h,
			       RPMTAG_CONFLICTNAME,
			       RPMTAG_CONFLICTVERSION,
			       RPMTAG_CONFLICTFLAGS);

		headerGetEntry(h, RPMTAG_BASENAMES, &type,
			       &basenames.p, &count);
		headerGetEntry(h, RPMTAG_DIRNAMES, &type,
			       &dirnames.p, &count);
		headerGetEntry(h, RPMTAG_DIRINDEXES, &type,
			       &dirindexes.p, &count);
		for (i = 0; i < count; i++) {
			snprintf(filename, sizeof filename, "%s%s",
				 dirnames.list[dirindexes.flags[i]],
				 basenames.list[i]);
			razor_importer_add_file(importer, filename);
		}

		razor_importer_finish_package(importer);

		printf("\rimporting %d", ++imported_count);
		fflush(stdout);
	}

	rpmdbClose(db);

	printf("\nsaving\n");
	return razor_importer_finish(importer);
}
