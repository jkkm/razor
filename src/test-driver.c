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
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <expat.h>

#include "razor.h"

#define XML_BUFFER_SIZE 4096

static void
parse_xml_file(const char *filename,
	       XML_StartElementHandler start,
	       XML_EndElementHandler end,
	       void *data)
{
	XML_Parser parser;
	char *buffer;
	int fd, len, err;

	parser = XML_ParserCreate(NULL);
	XML_SetElementHandler(parser, start, end);
	XML_SetUserData(parser, data);

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s: %m\n", filename);
		exit(-1);
	}

	while (1) {
		buffer = XML_GetBuffer(parser, XML_BUFFER_SIZE);
		len = read(fd, buffer, XML_BUFFER_SIZE);
		if (len == 0)
			break;
		err = XML_ParseBuffer(parser, len, len == 0);
		if (err == XML_STATUS_ERROR) {
			fprintf(stderr, "parse error at line %lu:\n%s\n",
				XML_GetCurrentLineNumber(parser),
				XML_ErrorString(XML_GetErrorCode(parser)));
			exit(-1);
		}
	}

	if (fd < 0) {
		fprintf(stderr, "read: %m\n");
		exit(-1);
	}

	close(fd);
}

struct test_context {
	struct razor_set *system_set, *repo_set, *result_set;

	struct razor_importer *importer;
	struct razor_set **importer_set;

	struct razor_transaction *trans;

	char *install_pkgs[3], *remove_pkgs[3];
	int n_install_pkgs, n_remove_pkgs;

	int unsat;
	int in_result;

	int debug, errors;
};

static void
get_atts(const char **atts, ...)
{
	va_list ap;
	const char *name, **ptr;
	int i;

	va_start(ap, atts);
	while (name = va_arg(ap, const char *), name != NULL) {
		ptr = va_arg(ap, const char **);
		*ptr = NULL;
		for (i = 0; atts[i]; i += 2) {
			if (strcmp(atts[i], name) == 0)
				*ptr = atts[i + 1];
		}
	}
	va_end(ap);
}

static enum razor_property_flags
parse_relation (const char *rel_str)
{
	if (!rel_str)
		return -1;
	if (rel_str[0] == 'L')
		return rel_str[1] == 'E' ? RAZOR_PROPERTY_LESS | RAZOR_PROPERTY_EQUAL : RAZOR_PROPERTY_LESS;
	else if (rel_str[0] == 'G')
		return rel_str[1] == 'E' ? RAZOR_PROPERTY_GREATER | RAZOR_PROPERTY_EQUAL : RAZOR_PROPERTY_GREATER;
	else if (rel_str[0] == 'E' || rel_str[1] == 'Q')
		return RAZOR_PROPERTY_EQUAL;
	else
		return -1;
}

static void
start_test(struct test_context *ctx, const char **atts)
{
	const char *name = NULL;

	get_atts(atts, "name", &name, NULL);
	if (!name) {
		fprintf(stderr, "Test with no name\n");
		exit(1);
	}
	printf("%s\n", name);
}

static void
end_test(struct test_context *ctx)
{
	if (ctx->system_set) {
		razor_set_destroy(ctx->system_set);
		ctx->system_set = NULL;
	}
	if (ctx->repo_set) {
		razor_set_destroy(ctx->repo_set);
		ctx->repo_set = NULL;
	}
	if (ctx->result_set) {
		razor_set_destroy(ctx->result_set);
		ctx->result_set = NULL;
	}
	if (ctx->trans) {
		razor_transaction_destroy(ctx->trans);
		ctx->trans = NULL;
	}
}

static void
start_set(struct test_context *ctx, const char **atts)
{
	const char *name = NULL;

	ctx->importer = razor_importer_create();
	get_atts(atts, "name", &name, NULL);
	if (!name)
		ctx->importer_set = &ctx->result_set;
	else if (!strcmp(name, "system"))
		ctx->importer_set = &ctx->system_set;
	else if (!strcmp(name, "repo"))
		ctx->importer_set = &ctx->repo_set;
	else {
		fprintf(stderr, "  bad set name '%s'\n", name);
		exit(1);
	}
}

static void
end_set(struct test_context *ctx)
{
	*ctx->importer_set = razor_importer_finish(ctx->importer);
	ctx->importer = NULL;
}

static void
start_package(struct test_context *ctx, const char **atts)
{
	const char *name = NULL, *version = NULL, *arch = NULL;

	get_atts(atts, "name", &name,
		 "version", &version,
		 "arch", &arch,
		 NULL);

	if (!name) {
		fprintf(stderr, "  package with no name\n");
		exit(1);
	}

	razor_importer_begin_package(ctx->importer, name, version, arch);
	razor_importer_add_property(ctx->importer, name,
				    RAZOR_PROPERTY_EQUAL | RAZOR_PROPERTY_PROVIDES,
				    version);
}

static void
end_package(struct test_context *ctx)
{
	razor_importer_finish_package(ctx->importer);
}

static void
add_property(struct test_context *ctx, enum razor_property_flags type, const char *name, enum razor_property_flags rel, const char *version)
{
	razor_importer_add_property(ctx->importer, name,
				    rel | type, version);
}

static const char*
razor_property_flags_relation_to_string(enum razor_property_flags rel)
{
	if (rel == RAZOR_PROPERTY_LESS)
		return "<";
	if (rel == (RAZOR_PROPERTY_EQUAL | RAZOR_PROPERTY_LESS))
		return "<=";
	if (rel == RAZOR_PROPERTY_EQUAL)
		return "=";
	if (rel == (RAZOR_PROPERTY_EQUAL | RAZOR_PROPERTY_GREATER))
		return ">=";
	if (rel == RAZOR_PROPERTY_GREATER)
		return ">";

	return "";
}

static void
check_unsatisfiable_property(struct test_context *ctx,
			     enum razor_property_flags type,
			     const char *name,
			     enum razor_property_flags rel,
			     const char *version)
{
	if (!version)
		version = "";

	if (razor_transaction_unsatisfied_property(ctx->trans,
						   name, rel | type, version))
		return;

	fprintf(stderr, "  didn't get unsatisfiable '%s %s %s'\n",
		name, razor_property_flags_relation_to_string(rel), version);
	ctx->errors++;
}

static void
start_property(struct test_context *ctx, enum razor_property_flags type, const char **atts)
{
	const char *name = NULL, *rel_str = NULL, *version = NULL;
	enum razor_property_flags rel;

	get_atts(atts, "name", &name, "relation", &rel_str, "version", &version, NULL);
	if (name == NULL) {
		fprintf(stderr, "  no name specified for property\n");
		exit(1);
	}
	if (version) {
		rel = parse_relation(rel_str);
		if (rel == -1) {
			fprintf(stderr, "  bad or missing version relation for property %s\n", name);
			exit(1);
		}
	} else
		rel = RAZOR_PROPERTY_EQUAL;

	if (ctx->unsat)
		check_unsatisfiable_property(ctx, type, name, rel, version);
	else
		add_property(ctx, type, name, rel, version);
}

static void
start_transaction(struct test_context *ctx, const char **atts)
{
	ctx->n_install_pkgs = 0;
	ctx->n_remove_pkgs = 0;
}

static struct razor_package *
get_package(struct razor_set *set, const char *package)
{
	struct razor_package_iterator *pi;
	struct razor_package *p;
	const char *name, *version, *arch;

	pi = razor_package_iterator_create(set);
	while (razor_package_iterator_next(pi, &p, RAZOR_DETAIL_NAME, &name,
					   RAZOR_DETAIL_VERSION, &version,
					   RAZOR_DETAIL_ARCH, &arch,
					   RAZOR_DETAIL_LAST)) {
		if (strcmp(package, name) == 0)
			break;
	}
	razor_package_iterator_destroy(pi);

	return p;
}

static void
end_transaction(struct test_context *ctx)
{
	struct razor_package *pkg;
	int errors, i;

	ctx->trans = razor_transaction_create(ctx->system_set, ctx->repo_set);
	for (i = 0; i < ctx->n_install_pkgs; i++) {
		pkg = get_package(ctx->repo_set, ctx->install_pkgs[i]);
		razor_transaction_install_package(ctx->trans, pkg);
	}
	for (i = 0; i < ctx->n_remove_pkgs; i++) {
		pkg = get_package(ctx->system_set, ctx->remove_pkgs[i]);
		if (!pkg)
			pkg = get_package(ctx->repo_set, ctx->remove_pkgs[i]);

		razor_transaction_remove_package(ctx->trans, pkg);
	}

	razor_transaction_resolve(ctx->trans);
	errors = razor_transaction_describe(ctx->trans);
	printf("\n");

	while (ctx->n_install_pkgs--)
		free(ctx->install_pkgs[ctx->n_install_pkgs]);
	while (ctx->n_remove_pkgs--)
		free(ctx->remove_pkgs[ctx->n_remove_pkgs]);

	if (!errors) {
		struct razor_set *new;
		new = razor_transaction_finish(ctx->trans);
		ctx->trans = NULL;
		ctx->system_set = new;
	}
}

static void
start_install_or_update(struct test_context *ctx, const char **atts)
{
	const char *name = NULL;

	get_atts(atts, "name", &name, NULL);
	if (!name) {
		fprintf(stderr, "  install/update with no name\n");
		exit(1);
	}

	ctx->install_pkgs[ctx->n_install_pkgs++] = strdup(name);
}

static void
start_remove(struct test_context *ctx, const char **atts)
{
	const char *name = NULL;

	get_atts(atts, "name", &name, NULL);
	if (!name) {
		fprintf(stderr, "  remove with no name\n");
		exit(1);
	}

	ctx->remove_pkgs[ctx->n_remove_pkgs++] = strdup(name);
}

static void
start_result(struct test_context *ctx, const char **atts)
{
	ctx->in_result = 1;
}

static void
diff_callback(enum razor_diff_action action,
	      struct razor_package *package,
	      const char *name,
	      const char *version,
	      const char *arch,
	      void *data)
{
	struct test_context *ctx = data;

	ctx->errors++;
	if (action == RAZOR_DIFF_ACTION_REMOVE) {
		fprintf(stderr, "  result set should not contain %s %s\n",
			name, version);
	} else {
		fprintf(stderr, "  result set should contain %s %s\n",
			name, version);
	}
}

static void
end_result(struct test_context *ctx)
{
	ctx->in_result = 0;

	if (ctx->result_set) {
		if (!ctx->system_set)
			ctx->system_set = razor_set_create();
		razor_set_diff(ctx->system_set, ctx->result_set,
			       diff_callback, ctx);
	}
}

static void
start_unsatisfiable(struct test_context *ctx, const char **atts)
{
	if (ctx->result_set) {
		fprintf(stderr, "Expected to fail, but didn't\n");
		exit(1);
	}

	ctx->unsat = 1;
}

static void
end_unsatisfiable(struct test_context *ctx)
{
	ctx->unsat = 0;
}

static void
start_test_element(void *data, const char *element, const char **atts)
{
	struct test_context *ctx = data;

	if (strcmp(element, "tests") == 0) {
		;
	} else if (strcmp(element, "test") == 0) {
		start_test(ctx, atts);
	} else if (strcmp(element, "set") == 0) {
		start_set(ctx, atts);
	} else if (strcmp(element, "transaction") == 0) {
		start_transaction(ctx, atts);
	} else if (strcmp(element, "install") == 0) {
		start_install_or_update(ctx, atts);
	} else if (strcmp(element, "install") == 0) {
		start_install_or_update(ctx, atts);
	} else if (strcmp(element, "remove") == 0) {
		start_remove(ctx, atts);
	} else if (strcmp(element, "result") == 0) {
		start_result(ctx, atts);
	} else if (strcmp(element, "unsatisfiable") == 0) {
		start_unsatisfiable(ctx, atts);
	} else if (strcmp(element, "package") == 0) {
		start_package(ctx, atts);
	} else if (strcmp(element, "requires") == 0) {
		start_property(ctx, RAZOR_PROPERTY_REQUIRES, atts);
	} else if (strcmp(element, "provides") == 0) {
		start_property(ctx, RAZOR_PROPERTY_PROVIDES, atts);
	} else if (strcmp(element, "conflicts") == 0) {
		start_property(ctx, RAZOR_PROPERTY_CONFLICTS, atts);
	} else if (strcmp(element, "obsoletes") == 0) {
		start_property(ctx, RAZOR_PROPERTY_OBSOLETES, atts);
	} else {
		fprintf(stderr, "Unrecognized element '%s'\n", element);
		exit(1);
	}
}

static void
end_test_element (void *data, const char *element)
{
	struct test_context *ctx = data;

	if (strcmp(element, "test") == 0) {
		end_test(ctx);
	} else if (strcmp(element, "set") == 0) {
		end_set(ctx);
	} else if (strcmp(element, "package") == 0) {
		end_package(ctx);
	} else if (strcmp(element, "transaction") == 0) {
		end_transaction(ctx);
	} else if (strcmp(element, "result") == 0) {
		end_result(ctx);
	} else if (strcmp(element, "unsatisfiable") == 0) {
		end_unsatisfiable(ctx);
	}
}

int main(int argc, char *argv[])
{
	struct test_context ctx;
	const char *test_file;

	memset(&ctx, 0, sizeof ctx);

	if (argc > 3) {
		fprintf(stderr, "usage: %s [-d] [TESTS-FILE]\n", argv[0]);
		exit(-1);
	}

	if (argc >= 2 && !strcmp (argv[1], "-d")) {
		ctx.debug = 1;
		argc--;
		argv++;
	}
	if (argc == 2)
		test_file = argv[1];
	else
		test_file = "test.xml";

	parse_xml_file(test_file, start_test_element, end_test_element, &ctx);

	if (ctx.errors) {
		fprintf(stderr, "\n%d errors\n", ctx.errors);
		return 1;
	} else
		return 0;
}
