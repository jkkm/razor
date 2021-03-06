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

#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <expat.h>
#include <zlib.h>
#include "razor.h"

/* Import a yum filelist as a razor package set. */

enum {
	YUM_STATE_BEGIN,
	YUM_STATE_PACKAGE_NAME,
	YUM_STATE_PACKAGE_ARCH,
	YUM_STATE_SUMMARY,
	YUM_STATE_DESCRIPTION,
	YUM_STATE_URL,
	YUM_STATE_LICENSE,
	YUM_STATE_CHECKSUM,
	YUM_STATE_REQUIRES,
	YUM_STATE_PROVIDES,
	YUM_STATE_OBSOLETES,
	YUM_STATE_CONFLICTS,
	YUM_STATE_FILE
};

struct yum_context {
	XML_Parser primary_parser;
	XML_Parser filelists_parser;
	XML_Parser current_parser;

	struct razor_importer *importer;
	struct import_property_context *current_property_context;
	char name[256], arch[64], summary[512], description[4096];
	char url[256], license[64], buffer[512], *p;
	char pkgid[128];
	uint32_t property_type;
	int state;

	int total, current;
};

static uint32_t
yum_to_razor_relation (const char *flags)
{
	if (flags[0] == 'L') {
		if (flags[1] == 'T')
			return RAZOR_PROPERTY_LESS;
		else
			return RAZOR_PROPERTY_LESS | RAZOR_PROPERTY_EQUAL;
	} else if (flags[0] == 'G') {
		if (flags[1] == 'T')
			return RAZOR_PROPERTY_GREATER;
		else
			return RAZOR_PROPERTY_GREATER | RAZOR_PROPERTY_EQUAL;
	} else
		return RAZOR_PROPERTY_EQUAL;
}

static void
yum_primary_start_element(void *data, const char *name, const char **atts)
{
	struct yum_context *ctx = data;
	const char *n, *epoch, *version, *release;
	char buffer[128];
	uint32_t pre, relation, flags;
	int i;

	if (strcmp(name, "metadata") == 0) {
		for (i = 0; atts[i]; i += 2) {
			if (strcmp(atts[i], "packages") == 0)
				ctx->total = atoi(atts[i + 1]);
		}
	} else if (strcmp(name, "name") == 0) {
		ctx->state = YUM_STATE_PACKAGE_NAME;
		ctx->p = ctx->name;
	} else if (strcmp(name, "arch") == 0) {
		ctx->state = YUM_STATE_PACKAGE_ARCH;
		ctx->p = ctx->arch;
	} else if (strcmp(name, "version") == 0) {
		epoch = NULL;
		version = NULL;
		release = NULL;
		for (i = 0; atts[i]; i += 2) {
			if (strcmp(atts[i], "epoch") == 0)
				epoch = atts[i + 1];
			else if (strcmp(atts[i], "ver") == 0)
				version = atts[i + 1];
			else if (strcmp(atts[i], "rel") == 0)
				release = atts[i + 1];
		}
		if (version == NULL || release == NULL) {
			fprintf(stderr, "invalid version tag, "
				"missing version or  release attribute\n");
			return;
		}

		razor_build_evr(buffer, sizeof buffer, epoch, version, release);
		razor_importer_begin_package(ctx->importer,
					     ctx->name, buffer, ctx->arch);
	} else if (strcmp(name, "summary") == 0) {
		ctx->p = ctx->summary;
		ctx->state = YUM_STATE_SUMMARY;
	} else if (strcmp(name, "description") == 0) {
		ctx->p = ctx->description;
		ctx->state = YUM_STATE_DESCRIPTION;
	} else if (strcmp(name, "url") == 0) {
		ctx->p = ctx->url;
		ctx->state = YUM_STATE_URL;
	} else if (strcmp(name, "checksum") == 0) {
		ctx->p = ctx->pkgid;
		ctx->state = YUM_STATE_CHECKSUM;
	} else if (strcmp(name, "rpm:license") == 0) {
		ctx->p = ctx->license;
		ctx->state = YUM_STATE_LICENSE;
	} else if (strcmp(name, "rpm:requires") == 0) {
		ctx->state = YUM_STATE_REQUIRES;
		ctx->property_type = RAZOR_PROPERTY_REQUIRES;
	} else if (strcmp(name, "rpm:provides") == 0) {
		ctx->state = YUM_STATE_PROVIDES;
		ctx->property_type = RAZOR_PROPERTY_PROVIDES;
	} else if (strcmp(name, "rpm:obsoletes") == 0) {
		ctx->state = YUM_STATE_OBSOLETES;
		ctx->property_type = RAZOR_PROPERTY_OBSOLETES;
	} else if (strcmp(name, "rpm:conflicts") == 0) {
		ctx->state = YUM_STATE_CONFLICTS;
		ctx->property_type = RAZOR_PROPERTY_CONFLICTS;
	} else if (strcmp(name, "rpm:entry") == 0 &&
		   ctx->state != YUM_STATE_BEGIN) {
		n = NULL;
		epoch = NULL;
		version = NULL;
		release = NULL;
		relation = RAZOR_PROPERTY_EQUAL;
		pre = 0;
		for (i = 0; atts[i]; i += 2) {
			if (strcmp(atts[i], "name") == 0)
				n = atts[i + 1];
			else if (strcmp(atts[i], "epoch") == 0)
				epoch = atts[i + 1];
			else if (strcmp(atts[i], "ver") == 0)
				version = atts[i + 1];
			else if (strcmp(atts[i], "rel") == 0)
				release = atts[i + 1];
			else if (strcmp(atts[i], "flags") == 0)
				relation = yum_to_razor_relation(atts[i + 1]);
			else if (strcmp(atts[i], "pre") == 0)
				pre = 
					RAZOR_PROPERTY_PRE |
					RAZOR_PROPERTY_POST |
					RAZOR_PROPERTY_PREUN |
					RAZOR_PROPERTY_POSTUN;
		}

		if (n == NULL) {
			fprintf(stderr, "invalid rpm:entry, "
				"missing name or version attributes\n");
			return;
		}

		razor_build_evr(buffer, sizeof buffer, epoch, version, release);
		flags = ctx->property_type | relation | pre;
		razor_importer_add_property(ctx->importer, n, flags, buffer);
	}
}

static void
yum_primary_end_element (void *data, const char *name)
{
	struct yum_context *ctx = data;

	switch (ctx->state) {
	case YUM_STATE_PACKAGE_NAME:
	case YUM_STATE_PACKAGE_ARCH:
	case YUM_STATE_SUMMARY:
	case YUM_STATE_DESCRIPTION:
	case YUM_STATE_URL:
	case YUM_STATE_LICENSE:
	case YUM_STATE_CHECKSUM:
	case YUM_STATE_FILE:
		ctx->state = YUM_STATE_BEGIN;
		break;
	}

	if (strcmp(name, "package") == 0) {
		razor_importer_add_details(ctx->importer, ctx->summary,
					   ctx->description, ctx->url,
					   ctx->license);

		XML_StopParser(ctx->current_parser, XML_TRUE);
		ctx->current_parser = ctx->filelists_parser;

		printf("\rimporting %d/%d", ++ctx->current, ctx->total);
		fflush(stdout);
	}
}

static void
yum_character_data (void *data, const XML_Char *s, int len)
{
	struct yum_context *ctx = data;

	switch (ctx->state) {
	case YUM_STATE_PACKAGE_NAME:
	case YUM_STATE_PACKAGE_ARCH:
	case YUM_STATE_SUMMARY:
	case YUM_STATE_DESCRIPTION:
	case YUM_STATE_URL:
	case YUM_STATE_LICENSE:
	case YUM_STATE_CHECKSUM:
	case YUM_STATE_FILE:
		memcpy(ctx->p, s, len);
		ctx->p += len;
		*ctx->p = '\0';
		break;
	}
}

static void
yum_filelists_start_element(void *data, const char *name, const char **atts)
{
	struct yum_context *ctx = data;
	const char *pkg, *pkgid;
	int i;

	if (strcmp(name, "package") == 0) {
		pkg = NULL;
		pkgid = NULL;
		for (i = 0; atts[i]; i += 2) {
			if (strcmp(atts[i], "name") == 0)
				pkg = atts[i + 1];
			else if (strcmp(atts[i], "pkgid") == 0)
				pkgid = atts[i + 1];
		}
		if (strcmp(pkgid, ctx->pkgid) != 0)
			fprintf(stderr, "primary.xml and filelists.xml "
				"mismatch for %s: %s vs %s",
				pkg, pkgid, ctx->pkgid);
	} else if (strcmp(name, "file") == 0) {
		ctx->state = YUM_STATE_FILE;
		ctx->p = ctx->buffer;
	}
}


static void
yum_filelists_end_element (void *data, const char *name)
{
	struct yum_context *ctx = data;

	ctx->state = YUM_STATE_BEGIN;
	if (strcmp(name, "package") == 0) {
		XML_StopParser(ctx->current_parser, XML_TRUE);
		ctx->current_parser = ctx->primary_parser;
		razor_importer_finish_package(ctx->importer);
	} else if (strcmp(name, "file") == 0)
		razor_importer_add_file(ctx->importer, ctx->buffer);

}

#define XML_BUFFER_SIZE 4096

struct razor_set *
razor_set_create_from_yum(void)
{
	struct yum_context ctx;
	void *buf;
	int len, ret;
	gzFile primary, filelists;
	XML_ParsingStatus status;

	ctx.importer = razor_importer_create();
	ctx.state = YUM_STATE_BEGIN;

	ctx.primary_parser = XML_ParserCreate(NULL);
	XML_SetUserData(ctx.primary_parser, &ctx);
	XML_SetElementHandler(ctx.primary_parser,
			      yum_primary_start_element,
			      yum_primary_end_element);
	XML_SetCharacterDataHandler(ctx.primary_parser,
				    yum_character_data);

	ctx.filelists_parser = XML_ParserCreate(NULL);
	XML_SetUserData(ctx.filelists_parser, &ctx);
	XML_SetElementHandler(ctx.filelists_parser,
			      yum_filelists_start_element,
			      yum_filelists_end_element);
	XML_SetCharacterDataHandler(ctx.filelists_parser,
				    yum_character_data);

	primary = gzopen("primary.xml.gz", "rb");
	if (primary == NULL)
		return NULL;
	filelists = gzopen("filelists.xml.gz", "rb");
	if (filelists == NULL)
		return NULL;

	ctx.current_parser = ctx.primary_parser;

	ctx.current = 0;

	do {
		XML_GetParsingStatus(ctx.current_parser, &status);
		switch (status.parsing) {
		case XML_SUSPENDED:
			ret = XML_ResumeParser(ctx.current_parser);
			break;
		case XML_PARSING:
		case XML_INITIALIZED:
			buf = XML_GetBuffer(ctx.current_parser,
					    XML_BUFFER_SIZE);
			if (ctx.current_parser == ctx.primary_parser)
				len = gzread(primary, buf, XML_BUFFER_SIZE);
			else
				len = gzread(filelists, buf, XML_BUFFER_SIZE);
			if (len < 0) {
				fprintf(stderr,
					"couldn't read input: %s\n",
					strerror(errno));
				return NULL;
			}

			XML_ParseBuffer(ctx.current_parser, len, len == 0);
			break;
		case XML_FINISHED:
			break;
		}
	} while (status.parsing != XML_FINISHED);


	XML_ParserFree(ctx.primary_parser);
	XML_ParserFree(ctx.filelists_parser);

	gzclose(primary);
	gzclose(filelists);

	printf ("\nsaving\n");
	return razor_importer_finish(ctx.importer);
}
