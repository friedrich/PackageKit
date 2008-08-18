/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Marcin Banasiak <megabajt@pld-linux.org>
 *
 * Licensed under the GNU General Public License Version 2
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <pk-backend.h>
#include <pk-package-ids.h>

#include <log.h>
#include <capreq.h>
#include <poldek.h>
#include <poclidek/poclidek.h>
#include <pkgdir/pkgdir.h>
#include <pkgdir/source.h>
#include <pkgu.h>
#include <pkgfl.h>
#include <pkgmisc.h>
#include <pm/pm.h>
#include <vfile/vfile.h>
#include <sigint/sigint.h>

static gchar* poldek_pkg_evr (const struct pkg *pkg);
static void poldek_backend_package (PkBackend *backend, struct pkg *pkg, PkInfoEnum infoenum, PkFilterEnum filters);
static long do_get_bytes_to_download (struct poldek_ts *ts, tn_array *pkgs);
static gint do_get_files_to_download (const struct poldek_ts *ts, const gchar *mark);
static void pb_load_packages (PkBackend *backend);
static void poldek_backend_set_allow_cancel (PkBackend *backend, gboolean allow_cancel, gboolean reset);

static void pb_error_show (PkBackend *backend, PkErrorCodeEnum errorcode);
static void pb_error_clean (void);
static void poldek_backend_percentage_data_destroy (PkBackend *backend);

typedef enum {
	TS_TYPE_ENUM_INSTALL,
	TS_TYPE_ENUM_UPDATE,
	TS_TYPE_ENUM_REFRESH_CACHE
} TsType;

enum {
	SEARCH_ENUM_NONE,
	SEARCH_ENUM_NAME,
	SEARCH_ENUM_GROUP,
	SEARCH_ENUM_DETAILS,
	SEARCH_ENUM_FILE,
	SEARCH_ENUM_PROVIDES,
	SEARCH_ENUM_RESOLVE
};

typedef struct {
	PkGroupEnum	group;
	const gchar	*regex;
} PLDGroupRegex;

static PLDGroupRegex group_perlre[] = {
	{PK_GROUP_ENUM_ACCESSORIES, "/.*Archiving\\|.*Dictionaries/"},
	{PK_GROUP_ENUM_ADMIN_TOOLS, "/.*Databases.*\\|.*Admin/"},
	{PK_GROUP_ENUM_COMMUNICATION, "/.*Communications/"},
	{PK_GROUP_ENUM_EDUCATION, "/.*Engineering\\|.*Math\\|.*Science/"},
	{PK_GROUP_ENUM_FONTS, "/Fonts/"},
	{PK_GROUP_ENUM_GAMES, "/.*Games.*/"},
	{PK_GROUP_ENUM_GRAPHICS, "/.*Graphics/"},
	{PK_GROUP_ENUM_LOCALIZATION, "/I18n/"},
	{PK_GROUP_ENUM_MULTIMEDIA, "/.*Multimedia\\|.*Sound/"},
	{PK_GROUP_ENUM_NETWORK, "/.*Networking.*\\|/.*Mail\\|.*News\\|.*WWW/"},
	{PK_GROUP_ENUM_OFFICE, "/.*Editors.*\\|.*Spreadsheets/"},
	{PK_GROUP_ENUM_OTHER, "/^Applications$\\|.*Console\\|.*Emulators\\|.*File\\|.*Printing\\|.*Terminal\\|.*Text\\|Documentation\\|^Libraries.*\\|^Themes.*\\|^X11$\\|.*Amusements\\|^X11\\/Applications$\\|^X11\\/Libraries$\\|.*Window\\ Managers.*/"},
	{PK_GROUP_ENUM_PROGRAMMING, "/.*Development.*/"},
	{PK_GROUP_ENUM_PUBLISHING, "/.*Publishing.*/"},
	{PK_GROUP_ENUM_SERVERS, "/Daemons\\|.*Servers/"},
	{PK_GROUP_ENUM_SYSTEM, "/.*Shells\\|.*System\\|Base.*/"},
	{0, NULL}
};

typedef struct {
	gint		step; // current step

	/* Numer of sources to update. It's used only by refresh cache,
	 * as each source can have multiple files to download. I don't
	 * know how to get numer of files which will be downloaded. */
	guint		nsources;

	long		bytesget;
	long		bytesdownload;

	/* how many files I have already downloaded or which I'm currently
	 * downloading */
	guint		filesget;
	/* how many files I have to download */
	guint		filesdownload;

	gint		percentage;
	gint		subpercentage;
} PercentageData;

typedef enum {
	PB_RPM_STATE_ENUM_NONE = 0,
	PB_RPM_STATE_ENUM_INSTALLING = (1 << 1),
	PB_RPM_STATE_ENUM_REPACKAGING = (1 << 2)
} PbRpmState;

/* I need this to avoid showing error messages more than once.
 * It's initalized by backend_initalize() and destroyed by
 * backend_destroy(), but every method should clean it at the
 * end. */
typedef struct {
	PbRpmState	rpmstate;

	/* last 'vfff: foo' message */
	gchar		*vfffmsg;

	/* all messages merged into one string which can
	 * be displayed at the end of transaction. */
	GString		*tslog;
} PbError;

/* global variables */
static gint verbose = 1;
static gint ref = 0;
static PbError *pberror;

static struct poldek_ctx	*ctx = NULL;
static struct poclidek_ctx	*cctx = NULL;

static gint
poldek_get_files_to_download (const struct poldek_ts *ts)
{
	gint	files = 0;

	files += do_get_files_to_download (ts, "I");
	files += do_get_files_to_download (ts, "D");

	return files;
}

static gint
do_get_files_to_download (const struct poldek_ts *ts, const gchar *mark)
{
	tn_array	*pkgs = NULL;
	gint		files = 0;

	pkgs = poldek_ts_get_summary (ts, mark);

	if (pkgs) {
		files = n_array_size (pkgs);
		n_array_free (pkgs);
	}

	return files;
}

/**
 * poldek_get_bytes_to_download:
 *
 * Returns: bytes to download
 */
static long
poldek_get_bytes_to_download (struct poldek_ts *ts, tn_array *pkgs)
{
	return do_get_bytes_to_download (ts, pkgs);
}

static long
poldek_get_bytes_to_download_from_ts (struct poldek_ts *ts)
{
	gchar mark[2][2] = {"I", "D"};
	long bytes = 0;
	gint i = 0;

	while (mark[i]) {
		tn_array *pkgs = poldek_ts_get_summary (ts, mark[i]);

		if (pkgs) {
			bytes += do_get_bytes_to_download (ts, pkgs);

			n_array_free (pkgs);
		}

		i++;
	}

	return bytes;
}

static long
do_get_bytes_to_download (struct poldek_ts *ts, tn_array *pkgs)
{
	gint i;
	long bytes = 0;

	for (i = 0; i < n_array_size (pkgs); i++) {
		struct pkg	*pkg = n_array_nth (pkgs, i);
		gchar		path[1024];

		if (pkg->pkgdir && (vf_url_type (pkg->pkgdir->path) & VFURL_REMOTE)) {
			if (pkg_localpath (pkg, path, sizeof(path), ts->cachedir)) {
				if (access(path, R_OK) != 0) {
					bytes += pkg->fsize;
				} else {
					if (!pm_verify_signature(ts->pmctx, path, PKGVERIFY_MD)) {
						bytes += pkg->fsize;
					}
				}
			}
		}
	}

	return bytes;
}

/**
 * VF_PROGRESS
 */
static void*
poldek_vf_progress_new (void *data, const gchar *label)
{
	PkBackend *backend = (PkBackend*) data;
	guint ts_type = pk_backend_get_uint (backend, "ts_type");

	if (ts_type == TS_TYPE_ENUM_INSTALL || ts_type == TS_TYPE_ENUM_UPDATE) {
		gchar		*filename = g_path_get_basename (label), *pkgname, *command;
		struct poclidek_rcmd *rcmd;
		tn_array	*pkgs = NULL;
		struct pkg	*pkg = NULL;

		pkgname = g_strndup (filename, (sizeof(gchar)*strlen(filename)-4));

		command = g_strdup_printf ("cd /all-avail; ls -q %s", pkgname);

		pk_backend_set_status (backend, PK_STATUS_ENUM_DOWNLOAD);

		rcmd = poclidek_rcmd_new (cctx, NULL);
		poclidek_rcmd_execline (rcmd, command);
		pkgs = poclidek_rcmd_get_packages (rcmd);

		if (pkgs) {
			pkg = n_array_nth (pkgs, 0);
			poldek_backend_package (backend, pkg, PK_INFO_ENUM_DOWNLOADING, PK_FILTER_ENUM_NONE);
		}

		poclidek_rcmd_free (rcmd);

		g_free (command);
		g_free (pkgname);
		g_free (filename);
	}

	return data;
}

static void
poldek_vf_progress (void *bar, long total, long amount)
{
	PkBackend	*backend = (PkBackend*) bar;
	PercentageData	*pd = pk_backend_get_pointer (backend, "percentage_ptr");
	guint ts_type = pk_backend_get_uint (backend, "ts_type");

	if (ts_type == TS_TYPE_ENUM_INSTALL || ts_type == TS_TYPE_ENUM_UPDATE) {
		float	frac = (float)amount / (float)total;

		/* file already downloaded */
		if (frac < 0) {
			pd->bytesget += total;
			pd->filesget++;

			pd->percentage = (gint)((float)(pd->bytesget) / (float)pd->bytesdownload * 100);
			pd->subpercentage = 100;
		} else {
			pd->percentage = (gint)(((float)(pd->bytesget + amount) / (float)pd->bytesdownload) * 100);
			pd->subpercentage = (gint)(frac * 100);
		}

		pk_backend_set_sub_percentage (backend, pd->subpercentage);

	} else if (ts_type == TS_TYPE_ENUM_REFRESH_CACHE) {
		if (pd->step == 0)
			pd->percentage = 1;
		else
			pd->percentage = (gint)(((float)pd->step / (float)pd->nsources) * 100);
	}

	pk_backend_set_percentage (backend, pd->percentage);
}

static void
poldek_vf_progress_reset (void *bar)
{
	PkBackend *backend = (PkBackend *) bar;
	PercentageData *pd = pk_backend_get_pointer (backend, "percentage_ptr");
	pd->subpercentage = 0;
}

/**
 * poldek_pkg_in_array_idx:
 *
 * Returns index of the first matching package. If not found, -1 will be returned.
 **/
static gint
poldek_pkg_in_array_idx (const struct pkg *pkg, const tn_array *array, tn_fn_cmp cmp_fn)
{
	gint	i;

	if (array) {
		for (i = 0; i < n_array_size (array); i++) {
			struct pkg	*p = n_array_nth (array, i);

			if (cmp_fn (pkg, p) == 0)
				return i;
		}
	}

	return -1;
}

static gboolean
poldek_pkg_in_array (const struct pkg *pkg, const tn_array *array, tn_fn_cmp cmp_fn)
{
	if (array == NULL)
		return FALSE;

	if (poldek_pkg_in_array_idx (pkg, array, cmp_fn) == -1)
		return FALSE;
	else
		return TRUE;
}

/**
 * ts_confirm:
 * Returns Yes - 1
 *	    No - 0
 */
static int
ts_confirm (void *data, struct poldek_ts *ts)
{
	tn_array	*ipkgs, *dpkgs, *rpkgs;
	PkBackend	*backend = (PkBackend *)data;
	gint		i = 0, result = 1;

	pk_debug ("START\n");

	ipkgs = poldek_ts_get_summary (ts, "I");
	dpkgs = poldek_ts_get_summary (ts, "D");
	rpkgs = poldek_ts_get_summary (ts, "R");

	if (poldek_ts_get_type (ts) == POLDEK_TS_TYPE_INSTALL) {
		tn_array *update_pkgs, *remove_pkgs, *install_pkgs;
		PercentageData *pd = pk_backend_get_pointer (backend, "percentage_ptr");
		guint to_install = 0;

		update_pkgs = n_array_new (4, (tn_fn_free)pkg_free, NULL);
		remove_pkgs = n_array_new (4, (tn_fn_free)pkg_free, NULL);
		install_pkgs = n_array_new (4, (tn_fn_free)pkg_free, NULL);

		pd->step = 0;

		pd->bytesget = 0;
		pd->bytesdownload = poldek_get_bytes_to_download_from_ts (ts);

		pd->filesget = 0;
		pd->filesdownload = poldek_get_files_to_download (ts);

		pberror->rpmstate = PB_RPM_STATE_ENUM_NONE;

		/* create an array with pkgs which will be updated */
		if (rpkgs) {
			for (i = 0; i < n_array_size (rpkgs); i++) {
				struct pkg	*rpkg = n_array_nth (rpkgs, i);

				if (poldek_pkg_in_array (rpkg, ipkgs, (tn_fn_cmp)pkg_cmp_name))
					n_array_push (update_pkgs, pkg_link (rpkg));
				else if (poldek_pkg_in_array (rpkg, dpkgs, (tn_fn_cmp)pkg_cmp_name))
					n_array_push (update_pkgs, pkg_link (rpkg));
				else
					n_array_push (remove_pkgs, pkg_link (rpkg));
			}
		}

		/* create an array with pkgs which will be installed */
		if (ipkgs) {
			for (i = 0; i < n_array_size (ipkgs); i++) {
				struct pkg *ipkg = n_array_nth (ipkgs, i);

				if (poldek_pkg_in_array (ipkg, update_pkgs, (tn_fn_cmp)pkg_cmp_name) == FALSE)
					n_array_push (install_pkgs, pkg_link (ipkg));
			}
		}
		if (dpkgs) {
			for (i = 0; i < n_array_size (dpkgs); i++) {
				struct pkg *dpkg = n_array_nth (dpkgs, i);

				if (poldek_pkg_in_array (dpkg, update_pkgs, (tn_fn_cmp)pkg_cmp_name) == FALSE)
					n_array_push (install_pkgs, pkg_link (dpkg));
			}
		}

		/* packages to install & update */
		to_install = n_array_size (install_pkgs);
		to_install += n_array_size (update_pkgs);

		pk_backend_set_uint (backend, "to_install", to_install);

		pk_backend_set_pointer (backend, "to_update_pkgs", update_pkgs);
		pk_backend_set_pointer (backend, "to_remove_pkgs", remove_pkgs);
		pk_backend_set_pointer (backend, "to_install_pkgs", install_pkgs);
	} else if (poldek_ts_get_type (ts) == POLDEK_TS_TYPE_UNINSTALL) {
		gboolean allow_deps = pk_backend_get_bool (backend, "allow_deps");

		/* check if transaction can be performed */
		if (allow_deps == FALSE) {
			if (dpkgs && n_array_size (dpkgs) > 0) {
				result = 0;
			}
		}

		if (result == 1) { /* remove is allowed */
			pk_backend_set_status (backend, PK_STATUS_ENUM_REMOVE);

			/* we shouldn't cancel remove proccess */
			poldek_backend_set_allow_cancel (backend, FALSE, FALSE);

			if (dpkgs) {
				for (i = 0; i < n_array_size (dpkgs); i++) {
					struct pkg *pkg = n_array_nth (dpkgs, i);

					poldek_backend_package (backend, pkg, PK_INFO_ENUM_REMOVING, PK_FILTER_ENUM_NONE);
				}
			}

			if (rpkgs) {
				for (i = 0; i < n_array_size (rpkgs); i++) {
					struct pkg *pkg = n_array_nth (rpkgs, i);

					poldek_backend_package (backend, pkg, PK_INFO_ENUM_REMOVING, PK_FILTER_ENUM_NONE);
				}
			}
		}
	}

	n_array_cfree (&ipkgs);
	n_array_cfree (&dpkgs);
	n_array_cfree (&rpkgs);

	return result;
}

/**
 * suggests_callback:
 **/
static gint
suggests_callback (void *data, const struct poldek_ts *ts, const struct pkg *pkg,
		   tn_array *caps, tn_array *choices, int hint)
{
	/* install all suggested packages */
	return 1;
}
/**
 * setup_vf_progress:
 */
static void
setup_vf_progress (struct vf_progress *vf_progress, PkBackend *backend)
{
	vf_progress->data = backend;
	vf_progress->new = poldek_vf_progress_new;
	vf_progress->progress = poldek_vf_progress;
	vf_progress->reset = poldek_vf_progress_reset;
	vf_progress->free = NULL;

	vfile_configure (VFILE_CONF_VERBOSE, &verbose);
	vfile_configure (VFILE_CONF_STUBBORN_NRETRIES, 5);

	poldek_configure (ctx, POLDEK_CONF_VFILEPROGRESS, vf_progress);
}

static gint
pkg_cmp_name_evr_rev_recno (const struct pkg *p1, const struct pkg *p2) {
	register gint rc;

	if ((rc = pkg_cmp_name_evr_rev (p1, p2)) == 0)
		rc = -(p1->recno - p2->recno);

	return rc;
}

static gboolean
pkg_is_installed (struct pkg *pkg)
{
	struct pkgdb *db;
	gint cmprc, is_installed = 0;
	struct poldek_ts *ts;

	g_return_val_if_fail (pkg != NULL, FALSE);

	/* XXX: I don't know how to get ctx->rootdir */
	ts = poldek_ts_new (ctx, 0);

	db = pkgdb_open (ts->pmctx, ts->rootdir, NULL, O_RDONLY, NULL);

	if (db) {
		is_installed = pkgdb_is_pkg_installed (db, pkg, &cmprc);

		pkgdb_free (db);
	}

	poldek_ts_free (ts);

	return is_installed ? TRUE : FALSE;
}

/**
 * poldek_get_security_updates:
 **/
static tn_array*
poldek_get_security_updates (void)
{
	struct poclidek_rcmd *rcmd = NULL;
	tn_array *pkgs = NULL;

	rcmd = poclidek_rcmd_new (cctx, NULL);

	poclidek_rcmd_execline (rcmd, "cd /all-avail; ls -S");

	pkgs = poclidek_rcmd_get_packages (rcmd);

	poclidek_rcmd_free (rcmd);

	return pkgs;
}

/**
 * pld_group_to_enum:
 *
 * Converts PLD RPM group to PkGroupEnum.
 **/
static PkGroupEnum
pld_group_to_enum (const gchar *group)
{
	g_return_val_if_fail (group != NULL, PK_GROUP_ENUM_OTHER);

	if (strstr (group, "Archiving") != NULL ||
	    strstr (group, "Dictionaries") != NULL)
		return PK_GROUP_ENUM_ACCESSORIES;
	else if (strstr (group, "Databases") != NULL ||
		 strstr (group, "Admin") != NULL)
		return PK_GROUP_ENUM_ADMIN_TOOLS;
	else if (strstr (group, "Communications") != NULL)
		return PK_GROUP_ENUM_COMMUNICATION;
	else if (strstr (group, "Engineering") != NULL ||
		 strstr (group, "Math") != NULL	||
		 strstr (group, "Science") != NULL)
		return PK_GROUP_ENUM_EDUCATION;
	else if (strcmp (group, "Fonts") == 0)
		return PK_GROUP_ENUM_FONTS;
	else if (strstr (group, "Games") != NULL)
		return PK_GROUP_ENUM_GAMES;
	else if (strstr (group, "Graphics") != NULL)
		return PK_GROUP_ENUM_GRAPHICS;
	else if (strcmp (group, "I18n") == 0)
		return PK_GROUP_ENUM_LOCALIZATION;
	else if (strstr (group, "Multimedia") != NULL ||
		 strstr (group, "Sound") != NULL)
		return PK_GROUP_ENUM_MULTIMEDIA;
	else if (strstr (group, "Networking") != NULL ||
		 strstr (group, "Mail") != NULL ||
		 strstr (group, "News") != NULL ||
		 strstr (group, "WWW") != NULL)
		return PK_GROUP_ENUM_NETWORK;
	else if (strstr (group, "Editors") != NULL ||
		 strstr (group, "Spreadsheets") != NULL)
		return PK_GROUP_ENUM_OFFICE;
	else if (strstr (group, "Development") != NULL)
		return PK_GROUP_ENUM_PROGRAMMING;
	else if (strstr (group, "Publishing") != NULL)
		return PK_GROUP_ENUM_PUBLISHING;
	else if (strstr (group, "Daemons") != NULL ||
		 strstr (group, "Servers") != NULL)
		return PK_GROUP_ENUM_SERVERS;
	else if (strstr (group, "Shells") != NULL ||
		 strstr (group, "System") != NULL ||
		 strstr (group, "Base") != NULL)
		return PK_GROUP_ENUM_SYSTEM;
	else
		return PK_GROUP_ENUM_OTHER;
}

/**
 * pld_group_get_regex_from_enum:
 **/
static const gchar*
pld_group_get_regex_from_enum (PkGroupEnum value)
{
	gint i;

	for (i = 0;; i++) {
		if (group_perlre[i].regex == NULL)
			return NULL;

		if (group_perlre[i].group == value)
			return group_perlre[i].regex;
	}
}

/**
 * poldek_pkg_evr:
 */
static gchar*
poldek_pkg_evr (const struct pkg *pkg)
{
	if (pkg->epoch == 0)
		return g_strdup_printf ("%s-%s", pkg->ver, pkg->rel);
	else
		return g_strdup_printf ("%d:%s-%s", pkg->epoch, pkg->ver, pkg->rel);
}

static gchar*
poldek_get_vr_from_package_id_evr (const gchar *evr)
{
	gchar		**sections, *result;

	sections = g_strsplit (evr, ":", 2);

	if (sections[1])
		result = g_strdup (sections[1]);
	else
		result = g_strdup (evr);

	g_strfreev (sections);

	return result;
}

/**
 * poldek_get_nvra_from_package_id:
 */
static gchar*
poldek_get_nvra_from_package_id (const gchar* package_id)
{
	PkPackageId	*pi;
	gchar		*vr, *result;

	pi = pk_package_id_new_from_string (package_id);
	vr = poldek_get_vr_from_package_id_evr (pi->version);

	result = g_strdup_printf ("%s-%s.%s", pi->name, vr, pi->arch);

	g_free (vr);
	pk_package_id_free (pi);

	return result;
}

/**
 * poldek_get_installed_packages:
 */
static tn_array*
poldek_get_installed_packages (void)
{
	struct poclidek_rcmd	*rcmd = NULL;
	tn_array		*arr = NULL;

	rcmd = poclidek_rcmd_new (cctx, NULL);
	poclidek_rcmd_execline (rcmd, "cd /installed; ls -q *");

	arr = poclidek_rcmd_get_packages (rcmd);

	poclidek_rcmd_free (rcmd);

	return arr;
}

static tn_array*
poldek_pkg_get_cves_from_pld_changelog (struct pkg *pkg, time_t since)
{
	struct pkguinf	*inf = NULL;
	const gchar	*ch;
	gchar *chlog = NULL;
	tn_array		*cves = NULL;

	if ((inf = pkg_uinf (pkg)) == NULL)
		return NULL;

	if ((ch = pkguinf_get_changelog (inf, since))) {
		chlog = g_strdup(ch);
		if (g_strstr_len (chlog, 55 * sizeof (gchar), " poldek@pld-linux.org\n- see ")) { /* pkg is subpackage */
			gchar *s, *e;

			s = strchr (chlog, '\n');

			s += 7; /* cut "\n- see " */

			if ((e = strchr (s, '\''))) {
				struct poclidek_rcmd *rcmd;
				gchar *command;

				*e = '\0'; /* now s is the name of package with changelog */

				rcmd = poclidek_rcmd_new (cctx, NULL);

				command = g_strdup_printf ("cd /all-avail; ls -q %s*", s);

				/* release it */
				g_free (chlog);
				chlog = NULL;

				if (poclidek_rcmd_execline (rcmd, command)) {
					tn_array *pkgs;

					pkgs = poclidek_rcmd_get_packages (rcmd);

					if (pkgs) {
						struct pkg *p = n_array_nth (pkgs, 0);
						struct pkguinf *inf_parent = NULL;

						if ((inf_parent = pkg_uinf (p))) {
							if ((ch = pkguinf_get_changelog (inf_parent, since)))
								chlog = g_strdup(ch);

							pkguinf_free (inf_parent);
						}
					}
					n_array_cfree (&pkgs);
				}

				poclidek_rcmd_free (rcmd);

				g_free (command);
			}
		}
	}

	if (chlog && strlen (chlog) > 0) {
		gchar *s=chlog;

		cves = n_array_new (2, free, (tn_fn_cmp)strcmp);
		while (1) {
			gchar cve[14];
			gboolean valid = TRUE;
			gint i;

			if ((s = strstr (s, "CVE-")) == NULL)
				break;

			if (strlen (s) < 13) /* CVE-XXXX-YYYY has 13 chars */
				break;

			for (i = 0; i < 14; i++) {
				if (i == 13)
					cve[i] = '\0';
				else
					cve[i] = *(s + i);
			}

			for (i = 4; i < 13; i++) {
				if (i == 8) {
					if (cve[i] != '-') {
						valid = FALSE;
						break;
					}
				} else if (g_ascii_isdigit (cve[i]) == FALSE) {
					valid = FALSE;
					break;
				}
			}

			if (valid)
				n_array_push (cves, g_strdup(cve));

			s += 13;
		}
	}

	pkguinf_free (inf);

	g_free (chlog);

	return cves;
}

static void
do_newest (tn_array *pkgs)
{
	guint	i = 1;

	if (!n_array_is_sorted (pkgs))
		n_array_sort_ex (pkgs, (tn_fn_cmp)pkg_cmp_name_evr_rev_recno);

	while (i < pkgs->items) {
		if (pkg_cmp_name (pkgs->data[i - 1], pkgs->data[i]) == 0) {
			struct pkg	*pkg = n_array_nth (pkgs, i);

			if (!pkg_is_installed (pkg)) {
				n_array_remove_nth (pkgs, i);
				continue;
			}
		}

		i++;
	}
}

/**
 * do_requires:
 */
static void
do_requires (tn_array *installed, tn_array *available, tn_array *requires,
	     struct pkg *pkg, PkBackend *backend)
{
	tn_array	*tmp = NULL;
	gint		i;
	PkFilterEnum filters;
	gboolean recursive;

	tmp = n_array_new (2, NULL, NULL);
	filters = pk_backend_get_uint (backend, "filters");

	/* if ~installed doesn't exists in filters, we can query installed */
	if (!pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
		for (i = 0; i < n_array_size (installed); i++) {
			struct pkg      *ipkg = n_array_nth (installed, i);
			int j;

			/* self match */
			if (pkg_cmp_name_evr (pkg, ipkg) == 0)
				continue;

			/* skip when there is no reqs */
			if (!ipkg->reqs)
				continue;

			/* package already added to the array */
			if (poldek_pkg_in_array (ipkg, requires, (tn_fn_cmp)pkg_cmp_name_evr_rev))
				continue;

			for (j = 0; j < n_array_size (ipkg->reqs); j++) {
				struct capreq   *req = n_array_nth (ipkg->reqs, j);

				if (capreq_is_rpmlib (req))
					continue;
				else if (capreq_is_file (req))
					continue;

				if (pkg_satisfies_req (pkg, req, 1)) {
					n_array_push (requires, pkg_link (ipkg));
					n_array_push (tmp, pkg_link (ipkg));
					break;
				}
			}
		}
	}
	if (!pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED)) {
		for (i = 0; i < n_array_size (available); i++) {
			struct pkg      *apkg = n_array_nth (available, i);
			int j;

			/* self match */
			if (pkg_cmp_name_evr (pkg, apkg) == 0)
				continue;

			if (!apkg->reqs)
				continue;

			/* package already added to the array */
			if (poldek_pkg_in_array (apkg, requires, (tn_fn_cmp)pkg_cmp_name_evr_rev))
				continue;

			for (j = 0; j < n_array_size (apkg->reqs); j++) {
				struct capreq   *req = n_array_nth (apkg->reqs, j);

				if (capreq_is_rpmlib (req))
					continue;
				else if (capreq_is_file (req))
					continue;

				if (pkg_satisfies_req (pkg, req, 1)) {
					n_array_push (requires, pkg_link (apkg));
					n_array_push (tmp, pkg_link (apkg));
					break;
				}
			}
		}
	}

	/* FIXME: recursive takes too much time for available packages, so don't use it */
	if (pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED)) {
		recursive = pk_backend_get_bool (backend, "recursive");
		if (recursive && tmp && n_array_size (tmp) > 0) {
			for (i = 0; i < n_array_size (tmp); i++) {
				struct pkg	*p = n_array_nth (tmp, i);
				do_requires (installed, available, requires, p, backend);
			}
		}
	}

	n_array_free (tmp);
}

/**
 * do_depends:
 */
static void
do_depends (tn_array *installed, tn_array *available, tn_array *depends, struct pkg *pkg, PkBackend *backend)
{
	tn_array	*reqs = pkg->reqs;
	tn_array	*tmp = NULL;
	gint		i;
	PkFilterEnum filters;
	gboolean recursive;

	tmp = n_array_new (2, NULL, NULL);
	filters = pk_backend_get_uint (backend, "filters");
	recursive = pk_backend_get_bool (backend, "recursive");

	/* nothing to do */
	if (!reqs || (reqs && n_array_size (reqs) < 1))
		return;

	for (i = 0; i < n_array_size (reqs); i++) {
		struct capreq	*req = n_array_nth (reqs, i);
		gboolean	found = FALSE;
		gint		j;

		/* skip it */
		if (capreq_is_rpmlib (req))
			continue;

		/* FIXME: pkg_satisfies_req() doesn't find file capreq's
		 * in installed packages, so skip them */
		if (capreq_is_file (req))
			continue;

		/* self match */
		if (pkg_satisfies_req (pkg, req, 1))
			continue;

		/* Maybe this capreq is satisfied by package already added to
		 * depends array. */
		for (j = 0; j < n_array_size (depends); j++) {
			struct pkg	*p = n_array_nth (depends, j);

			if (pkg_satisfies_req (p, req, 1)) {
				/* Satisfied! */
				found = TRUE;
				break;
			}
		}

		if (found)
			continue;

		/* first check in installed packages */
		if (!pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
			for (j = 0; j < n_array_size (installed); j++) {
				struct pkg	*p = n_array_nth (installed, j);

				if (pkg_satisfies_req (p, req, 1)) {
					found = TRUE;
					n_array_push (depends, pkg_link (p));
					n_array_push (tmp, pkg_link (p));
					break;
				}
			}
		}

		if (found)
			continue;

		/* ... now available */
		if (!pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED)) {
			for (j = 0; j < n_array_size (available); j++) {
				struct pkg	*p = n_array_nth (available, j);

				if (pkg_satisfies_req (p, req, 1)) {
					/* If only available packages are queried,
					 * don't return these, which are installed.
					 * Can be used to tell the user which packages
					 * will be additionaly installed. */
					if (pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
						gint	ret;

						ret = poldek_pkg_in_array_idx (p, installed, (tn_fn_cmp)pkg_cmp_name);

						if (ret >= 0) {
							struct pkg	*ipkg = NULL;

							ipkg = n_array_nth (installed, ret);

							if (pkg_satisfies_req (ipkg, req, 1))
								break;
						}
					}

					n_array_push (depends, pkg_link (p));
					n_array_push (tmp, pkg_link (p));
					break;
				}
			}
		}
	}

	if (recursive && tmp && n_array_size (tmp) > 0) {
		for (i = 0; i < n_array_size (tmp); i++) {
			struct pkg	*p = n_array_nth (tmp, i);

			do_depends (installed, available, depends, p, backend);
		}
	}

	n_array_free (tmp);
}

static gchar*
package_id_from_pkg (struct pkg *pkg, const gchar *repo, PkFilterEnum filters)
{
	gchar *evr, *package_id, *poldek_dir;

	g_return_val_if_fail (pkg != NULL, NULL);

	evr = poldek_pkg_evr (pkg);

	if (repo) {
		poldek_dir = g_strdup (repo);
	} else {
		/* when filters contain PK_FILTER_ENUM_NOT_INSTALLED package
		 * can't be marked as installed */
		if (!pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED) &&
		    pkg_is_installed (pkg)) {
			poldek_dir = g_strdup ("installed");
		} else {
			if (pkg->pkgdir && pkg->pkgdir->name) {
				poldek_dir = g_strdup (pkg->pkgdir->name);
			} else {
				poldek_dir = g_strdup ("all-avail");
			}
		}
	}

	package_id = pk_package_id_build (pkg->name,
					  evr,
					  pkg_arch (pkg),
					  poldek_dir);

	g_free (evr);
	g_free (poldek_dir);

	return package_id;
}

/**
 * poldek_backend_package:
 */
static void
poldek_backend_package (PkBackend *backend, struct pkg *pkg, PkInfoEnum infoenum, PkFilterEnum filters)
{
	struct pkguinf	*pkgu;
	gchar		*package_id;

	if (infoenum == PK_INFO_ENUM_UNKNOWN) {
		if (pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED)) {
			infoenum = PK_INFO_ENUM_INSTALLED;
		} else if (pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
			infoenum = PK_INFO_ENUM_AVAILABLE;
		} else {
			if (pkg_is_installed (pkg)) {
				infoenum = PK_INFO_ENUM_INSTALLED;
			} else {
				infoenum = PK_INFO_ENUM_AVAILABLE;
			}
		}
	}

	package_id = package_id_from_pkg (pkg, NULL, filters);

	pkgu = pkg_uinf (pkg);

	if (pkgu) {
		pk_backend_package (backend, infoenum, package_id, pkguinf_get (pkgu, PKGUINF_SUMMARY));
		pkguinf_free (pkgu);
	} else {
		pk_backend_package (backend, infoenum, package_id, "");
	}

	g_free (package_id);
}

/**
 * poldek_get_pkg_from_package_id:
 */
static struct pkg*
poldek_get_pkg_from_package_id (const gchar *package_id)
{
	PkPackageId		*pi;
	struct poclidek_rcmd	*rcmd;
	struct pkg		*result = NULL;
	gchar			*vr, *command;

	pi = pk_package_id_new_from_string (package_id);

	rcmd = poclidek_rcmd_new (cctx, NULL);

	vr = poldek_get_vr_from_package_id_evr (pi->version);
	command = g_strdup_printf ("cd /%s; ls -q %s-%s.%s", pi->data, pi->name, vr, pi->arch);

	if (poclidek_rcmd_execline (rcmd, command)) {
		tn_array	*pkgs = NULL;

		pkgs = poclidek_rcmd_get_packages (rcmd);

		if (n_array_size (pkgs) > 0) {
			/* only one package is needed */
			result = pkg_link (n_array_nth (pkgs, 0));
		}
		n_array_free (pkgs);
	}

	poclidek_rcmd_free (rcmd);

	pk_package_id_free (pi);

	g_free (vr);
	g_free (command);

	return result;
}

/**
 * poldek_pkg_is_devel:
 */
static gboolean
poldek_pkg_is_devel (struct pkg *pkg)
{
	if (g_str_has_suffix (pkg->name, "-devel"))
		return TRUE;
	if (g_str_has_suffix (pkg->name, "-debuginfo"))
		return TRUE;

	return FALSE;
}

/**
 * poldek_pkg_is_gui:
 */
static gboolean
poldek_pkg_is_gui (struct pkg *pkg)
{
	if (g_str_has_prefix (pkg_group (pkg), "X11"))
		return TRUE;

	return FALSE;
}

/**
 * search_package_thread:
 */
static gboolean
search_package_thread (PkBackend *backend)
{
	PkFilterEnum		filters;
	PkProvidesEnum		provides;
	gchar			*search_cmd = NULL;
	struct poclidek_rcmd	*cmd = NULL;
	const gchar *search;
	guint mode;

	pb_load_packages (backend);

	cmd = poclidek_rcmd_new (cctx, NULL);

	mode = pk_backend_get_uint (backend, "mode");
	search = pk_backend_get_string (backend, "search");
	filters = pk_backend_get_uint (backend, "filters");

	/* GetPackages */
	if (mode == SEARCH_ENUM_NONE) {
		search_cmd = g_strdup ("ls -q");
	/* SearchName */
	} else if (mode == SEARCH_ENUM_NAME) {
		search_cmd = g_strdup_printf ("ls -q *%s*", search);
	/* SearchGroup */
	} else if (mode == SEARCH_ENUM_GROUP) {
		PkGroupEnum	group;
		const gchar	*regex;

		group = pk_group_enum_from_text (search);
		regex = pld_group_get_regex_from_enum (group);

		search_cmd = g_strdup_printf ("search -qg --perlre %s", regex);
	/* SearchDetails */
	} else if (mode == SEARCH_ENUM_DETAILS) {
		search_cmd = g_strdup_printf ("search -dsq *%s*", search);
	/* SearchFile */
	} else if (mode == SEARCH_ENUM_FILE) {
		search_cmd = g_strdup_printf ("search -qlf *%s*", search);
	/* WhatProvides */
	} else if (mode == SEARCH_ENUM_PROVIDES) {
		provides = pk_backend_get_uint (backend, "provides");

		if (provides == PK_PROVIDES_ENUM_ANY) {
			search_cmd = g_strdup_printf ("search -qp %s", search);
		} else if (provides == PK_PROVIDES_ENUM_MODALIAS) {
		} else if (provides == PK_PROVIDES_ENUM_CODEC) {
		} else if (provides == PK_PROVIDES_ENUM_MIMETYPE) {
			search_cmd = g_strdup_printf ("search -qp mimetype(%s)", search);
		}
	} else if (mode == SEARCH_ENUM_RESOLVE) {
		gchar **package_ids;

		package_ids = pk_backend_get_strv (backend, "package_ids");

		search_cmd = g_strdup_printf ("ls -q %s", package_ids[0]);
	}

	if (cmd != NULL && search_cmd)
	{
		gchar		*command;
		tn_array	*pkgs = NULL, *installed = NULL, *available = NULL;

		if (!pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
			command = g_strdup_printf ("cd /installed; %s", search_cmd);
			if (poclidek_rcmd_execline (cmd, command)) {
				installed = poclidek_rcmd_get_packages (cmd);
			}

			g_free (command);
		}
		if (!pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED)) {
			command = g_strdup_printf ("cd /all-avail; %s", search_cmd);
			if (poclidek_rcmd_execline (cmd, command))
				available = poclidek_rcmd_get_packages (cmd);

			g_free (command);
		}

		if (!pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED) &&
		    !pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED) &&
		    installed && available) {
			gint	i;

			pkgs = installed;

			for (i = 0; i < n_array_size (available); i++) {
				struct pkg	*pkg = n_array_nth (available, i);

				/* check for duplicates */
				if (!poldek_pkg_in_array (pkg, pkgs, (tn_fn_cmp)pkg_cmp_name_evr)) {
					n_array_push (pkgs, pkg_link (pkg));
				}
			}

			n_array_sort_ex(pkgs, (tn_fn_cmp)pkg_cmp_name_evr_rev_recno);

			n_array_free (available);
		} else if (pk_enums_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED) || available) {
			pkgs = available;
		} else if (pk_enums_contain (filters, PK_FILTER_ENUM_INSTALLED) || installed)
			pkgs = installed;
		if (pkgs) {
			gint	i;

			if (pk_enums_contain (filters, PK_FILTER_ENUM_NEWEST))
				do_newest (pkgs);

			for (i = 0; i < n_array_size (pkgs); i++) {
				struct pkg	*pkg = n_array_nth (pkgs, i);

				if (sigint_reached ())
					break;

				/* check if we have to do development filtering
				 * (devel or ~devel in filters) */
				if (pk_enums_contain (filters, PK_FILTER_ENUM_DEVELOPMENT) ||
				    pk_enums_contain (filters, PK_FILTER_ENUM_NOT_DEVELOPMENT)) {
					if (pk_enums_contain (filters, PK_FILTER_ENUM_DEVELOPMENT)) {
						/* devel in filters */
						if (!poldek_pkg_is_devel (pkg))
							continue;
					} else {
						/* ~devel in filters */
						if (poldek_pkg_is_devel (pkg))
							continue;
					}
				}

				/* check if we have to do gui filtering
				 * (gui or ~gui in filters) */
				if (pk_enums_contain (filters, PK_FILTER_ENUM_GUI) ||
				    pk_enums_contain (filters, PK_FILTER_ENUM_NOT_GUI)) {
					if (pk_enums_contain (filters, PK_FILTER_ENUM_GUI)) {
						/* gui in filters */
						if (!poldek_pkg_is_gui (pkg))
							continue;
					} else {
						/* ~gui in filters */
						if (poldek_pkg_is_gui (pkg))
							continue;
					}
				}

				poldek_backend_package (backend, pkg, PK_INFO_ENUM_UNKNOWN, filters);
			}
			n_array_free (pkgs);
		} else {
			pk_backend_error_code (backend, PK_ERROR_ENUM_PACKAGE_NOT_FOUND, "Package not found");
		}
		poclidek_rcmd_free (cmd);
	}

	if (sigint_reached ()) {
		switch (mode) {
			case SEARCH_ENUM_NAME:
			case SEARCH_ENUM_GROUP:
			case SEARCH_ENUM_DETAILS:
			case SEARCH_ENUM_FILE:
			case SEARCH_ENUM_RESOLVE:
				pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_CANCELLED, "Search cancelled.");
				break;
			default:
				pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_CANCELLED, "Transaction cancelled.");
		}
	}

	g_free (search_cmd);

	pk_backend_finished (backend);
	return TRUE;
}

static gboolean
update_packages_thread (PkBackend *backend)
{
	struct vf_progress	vf_progress;
	gboolean		update_system;
	guint			i, toupdate = 0;
	gchar **package_ids, *command;
	GString *cmd;

	update_system = pk_backend_get_bool (backend, "update_system");
	package_ids = pk_backend_get_strv (backend, "package_ids");

	/* sth goes wrong. package_ids has to be set in UpdatePackages */
	if (update_system == FALSE && package_ids == NULL) {
		pk_warning ("package_ids cannot be NULL in UpdatePackages method.");
		pk_backend_finished (backend);
		return TRUE;
	}

	setup_vf_progress (&vf_progress, backend);

	pb_load_packages (backend);

	pk_backend_set_status (backend, PK_STATUS_ENUM_DEP_RESOLVE);
	pb_error_clean ();

	cmd = g_string_new ("upgrade ");

	if (update_system) {
		struct poclidek_rcmd *rcmd;

		rcmd = poclidek_rcmd_new (cctx, NULL);

		if (poclidek_rcmd_execline (rcmd, "cd /all-avail; ls -q -u")) {
			tn_array *pkgs;

			pkgs = poclidek_rcmd_get_packages (rcmd);

			/* UpdateSystem updates to the newest available packages */
			do_newest (pkgs);

			for (i = 0; i < n_array_size (pkgs); i++) {
				struct pkg *pkg;

				pkg = n_array_nth (pkgs, i);

				/* don't try to update blocked packages */
				if (!(pkg->flags & PKG_HELD)) {
					g_string_append_printf (cmd, "%s-%s-%s.%s ", pkg->name,
								pkg->ver, pkg->rel, pkg_arch (pkg));

					toupdate++;
				}
			}

			n_array_free (pkgs);
		}

		poclidek_rcmd_free (rcmd);
	} else {
		for (i = 0; i < g_strv_length (package_ids); i++) {
			struct pkg *pkg;

			pkg = poldek_get_pkg_from_package_id (package_ids[i]);

			g_string_append_printf (cmd, "%s-%s-%s.%s ", pkg->name,
						pkg->ver, pkg->rel, pkg_arch (pkg));

			toupdate++;

			pkg_free (pkg);
		}
	}

	command = g_string_free (cmd, FALSE);

	if (toupdate > 0) {
		struct poclidek_rcmd *rcmd;
		struct poldek_ts *ts;

		ts = poldek_ts_new (ctx, 0);
		rcmd = poclidek_rcmd_new (cctx, ts);

		ts->setop(ts, POLDEK_OP_PARTICLE, 0);

		if (!poclidek_rcmd_execline (rcmd, command))
			pb_error_show (backend, PK_ERROR_ENUM_TRANSACTION_ERROR);

		poclidek_rcmd_free (rcmd);
		poldek_ts_free (ts);
	}

	poldek_backend_percentage_data_destroy (backend);

	g_free (command);

	pk_backend_finished (backend);
	return TRUE;
}

static void
pb_load_packages (PkBackend *backend)
{
	gboolean	allow_cancel = pk_backend_get_allow_cancel (backend);

	/* this operation can't be cancelled, so if enabled, set allow_cancel to FALSE */
	if (allow_cancel)
		poldek_backend_set_allow_cancel (backend, FALSE, FALSE);

	/* load information about installed and available packages */
	poclidek_load_packages (cctx, POCLIDEK_LOAD_ALL);

	if (allow_cancel)
		poldek_backend_set_allow_cancel (backend, TRUE, FALSE);
}

static void
pb_error_show (PkBackend *backend, PkErrorCodeEnum errorcode)
{
	if (sigint_reached()) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_CANCELLED, "Action cancelled.");
		return;
	}

	/* Before emiting error_code try to find the most suitable PkErrorCodeEnum */
	if (g_strrstr (pberror->tslog->str, " unresolved depend") != NULL)
		errorcode = PK_ERROR_ENUM_DEP_RESOLUTION_FAILED;
	else if (g_strrstr (pberror->tslog->str, " conflicts") != NULL)
		errorcode = PK_ERROR_ENUM_FILE_CONFLICTS;

	pk_backend_error_code (backend, errorcode, pberror->tslog->str);
}

/**
 * pb_error_check:
 *
 * When we try to install already installed package, poldek won't report any error
 * just show message like 'liferea-1.4.11-2.i686: equal version installed, skipped'.
 * This function checks if it happens and if yes, emits error_code and returns TRUE.
 **/
static gboolean
pb_error_check (PkBackend *backend)
{
	PkErrorCodeEnum	errorcode = PK_ERROR_ENUM_UNKNOWN;

	if (g_strrstr (pberror->tslog->str, " version installed, skipped") != NULL)
		errorcode = PK_ERROR_ENUM_PACKAGE_ALREADY_INSTALLED;

	if (errorcode != PK_ERROR_ENUM_UNKNOWN) {
		pk_backend_error_code (backend, errorcode, pberror->tslog->str);
		return TRUE;
	}

	return FALSE;
}

static void
pb_error_clean (void)
{
	g_free (pberror->vfffmsg);

	pberror->tslog = g_string_erase (pberror->tslog, 0, -1);
	pberror->rpmstate = PB_RPM_STATE_ENUM_NONE;
}

static gint
pkg_n_strncmp (struct pkg *p, gchar *name)
{
	g_return_val_if_fail (p != NULL, -1);
	g_return_val_if_fail (p->name != NULL, -1);
	g_return_val_if_fail (name != NULL, 1);

	return strncmp (p->name, name, strlen (name));
}

static void
show_rpm_progress (PkBackend *backend, gchar *message)
{
	g_return_if_fail (message != NULL);

	if (pberror->rpmstate & PB_RPM_STATE_ENUM_REPACKAGING) {
		pk_debug ("repackaging '%s'", message);
	} else if (pberror->rpmstate & PB_RPM_STATE_ENUM_INSTALLING) {
		tn_array *upkgs, *ipkgs, *rpkgs, *arr = NULL;
		guint to_install;
		PkInfoEnum pkinfo;
		gint n = -2;

		pk_debug ("installing or updating '%s'", message);

		to_install = pk_backend_get_uint (backend, "to_install");

		ipkgs = pk_backend_get_pointer (backend, "to_install_pkgs");
		upkgs = pk_backend_get_pointer (backend, "to_update_pkgs");
		rpkgs = pk_backend_get_pointer (backend, "to_remove_pkgs");

		/* emit remove for packages marked for removal */
		if (rpkgs) {
			gint i;

			/* XXX: don't release rpkgs array here! */
			for (i = 0; i < n_array_size (rpkgs); i++) {
				struct pkg *pkg = n_array_nth (rpkgs, i);

				poldek_backend_package (backend, pkg, PK_INFO_ENUM_REMOVING, PK_FILTER_ENUM_NONE);

				n_array_remove_nth (rpkgs, i);
			}
		}

		if (upkgs) {
			n = n_array_bsearch_idx_ex (upkgs, message, (tn_fn_cmp)pkg_n_strncmp);
		}

		if (n >= 0) {
			pkinfo = PK_INFO_ENUM_UPDATING;
			arr = upkgs;
		} else if (ipkgs) {
			n = n_array_bsearch_idx_ex (ipkgs, message, (tn_fn_cmp)pkg_n_strncmp);

			if (n >= 0) {
				pkinfo = PK_INFO_ENUM_INSTALLING;
				arr = ipkgs;
			}
		}

		if (arr) {
			struct pkg *pkg = n_array_nth (arr, n);
			guint in_arrays = 0;

			poldek_backend_package (backend, pkg, pkinfo, PK_FILTER_ENUM_NONE);

			n_array_remove_nth (arr, n);

			if (upkgs) {
				in_arrays += n_array_size (upkgs);
			}
			if (ipkgs) {
				in_arrays += n_array_size (ipkgs);
			}

			pk_backend_set_percentage (backend, (gint)(((float)(to_install - in_arrays) / (float)to_install) * 100));
		}
	}
}

/* Returns NULL if not found */
static gchar*
get_filename_from_message (char *message)
{
	gchar *msg = NULL, *p;

	if ((p = strchr (message, ':')) == NULL)
		return NULL;

	/* check if it's really rpm progress
	 * example: ' 4:foo    ###'
	 */
	if (g_ascii_isdigit (*(p - 1))) {
		p++;

		msg = p;

		while (p) {
			if (*p == '#' || g_ascii_isspace (*p)) {
				*p = '\0';
				break;
			}

			p++;
		}
	}

	return msg;
}

static void
poldek_backend_log (void *data, int pri, char *message)
{
	PkBackend *backend = (PkBackend*)data;

	/* skip messages that we don't want to show */
	if (g_str_has_prefix (message, "Nothing")) // 'Nothing to do'
		return;
	if (g_str_has_prefix (message, "There we")) // 'There were errors'
		return;

	/* catch vfff messages */
	if (g_str_has_prefix (message, "vfff: ")) {
		if (g_str_has_prefix (message + 6, "Inter")) // 'Interrupted system call'
			return;
		else if (g_str_has_prefix (message + 6, "connection cancell")) // 'connection cancelled'
			return;

		/* check if this message was already showed */
		if (pberror->vfffmsg) {
			if (strcmp (pberror->vfffmsg, message) == 0)
				return;
			else
				g_free (pberror->vfffmsg);
		}

		pberror->vfffmsg = g_strdup (message);

		// 'vfff: unable to connect to ftp.pld-linux.org:21: Connection refused'
		pk_backend_message (backend, PK_MESSAGE_ENUM_WARNING, "%s", message);
	} else {
		if (pri & LOGERR) {
			g_string_append_printf (pberror->tslog, "error: %s", message);
		} else {
			g_string_append_printf (pberror->tslog, "%s", message);
		}
	}

	if (strstr (message, "Preparing...")) {
		pberror->rpmstate |= PB_RPM_STATE_ENUM_INSTALLING;

		/* we shouldn't cancel install / update proccess */
		poldek_backend_set_allow_cancel (backend, FALSE, FALSE);
	} else if (strstr (message, "Repackaging...")) {
		pberror->rpmstate |= PB_RPM_STATE_ENUM_REPACKAGING;

		pk_backend_set_status (backend, PK_STATUS_ENUM_REPACKAGING);
	} else if (strstr (message, "Upgrading...")) {
		pberror->rpmstate &= (~PB_RPM_STATE_ENUM_REPACKAGING);
	}
	if (pberror->rpmstate != PB_RPM_STATE_ENUM_NONE) {
		gchar *fn;

		if ((fn = get_filename_from_message (message)) == NULL)
			return;

		if ((pberror->rpmstate & PB_RPM_STATE_ENUM_REPACKAGING) == FALSE) {
			guint ts_type = pk_backend_get_uint (backend, "ts_type");

			/* set proper status */
			if (ts_type == TS_TYPE_ENUM_INSTALL) {
				pk_backend_set_status (backend, PK_STATUS_ENUM_INSTALL);
			} else if (ts_type == TS_TYPE_ENUM_UPDATE) {
				pk_backend_set_status (backend, PK_STATUS_ENUM_UPDATE);
			}
		}

		show_rpm_progress (backend, fn);
	}
}

static void
poldek_backend_set_allow_cancel (PkBackend *backend, gboolean allow_cancel, gboolean reset)
{
	if (reset)
		sigint_reset ();

	pk_backend_set_allow_cancel (backend, allow_cancel);
}

static void
poldek_backend_percentage_data_create (PkBackend *backend)
{
	PercentageData *data;

	data = g_new0 (PercentageData, 1);
	pk_backend_set_pointer (backend, "percentage_ptr", data);
}

static void
poldek_backend_percentage_data_destroy (PkBackend *backend)
{
	PercentageData *data;
	tn_array *upkgs, *ipkgs, *rpkgs;

	data = (gpointer) pk_backend_get_pointer (backend, "percentage_ptr");

	upkgs = (gpointer) pk_backend_get_pointer (backend, "to_update_pkgs");
	ipkgs = (gpointer) pk_backend_get_pointer (backend, "to_install_pkgs");
	rpkgs = (gpointer) pk_backend_get_pointer (backend, "to_remove_pkgs");

	n_array_cfree (&upkgs);
	n_array_cfree (&ipkgs);
	n_array_cfree (&rpkgs);

	g_free (data);
}

static void
do_poldek_init (PkBackend *backend)
{
	poldeklib_init ();

	ctx = poldek_new (0);

	poldek_load_config (ctx, "/etc/poldek/poldek.conf", NULL, 0);

	poldek_setup (ctx);

	cctx = poclidek_new (ctx);

	poldek_set_verbose (1);
	/* disable LOGFILE and LOGTTY logging */
	poldek_configure (ctx, POLDEK_CONF_LOGFILE, NULL);
	poldek_configure (ctx, POLDEK_CONF_LOGTTY, NULL);

	poldek_log_set_appender ("PackageKit", (void *)backend, NULL, 0, (poldek_vlog_fn)poldek_backend_log);

	/* disable unique package names */
	poldek_configure (ctx, POLDEK_CONF_OPT, POLDEK_OP_UNIQN, 0);

	/* poldek has to ask. Otherwise callbacks won't be used */
	poldek_configure (ctx, POLDEK_CONF_OPT, POLDEK_OP_CONFIRM_INST, 1);
	poldek_configure (ctx, POLDEK_CONF_OPT, POLDEK_OP_CONFIRM_UNINST, 1);
	/* (...), but we don't need choose_equiv callback */
	poldek_configure (ctx, POLDEK_CONF_OPT, POLDEK_OP_EQPKG_ASKUSER, 0);

	poldek_configure (ctx, POLDEK_CONF_TSCONFIRM_CB, ts_confirm, backend);
	/* Install all suggested packages by default */
	poldek_configure (ctx, POLDEK_CONF_CHOOSESUGGESTS_CB, suggests_callback, NULL);

	sigint_init ();
}

static void
do_poldek_destroy (PkBackend *backend)
{
	sigint_destroy ();

	poclidek_free (cctx);
	poldek_free (ctx);

	poldeklib_destroy ();
}

static void
poldek_reload (PkBackend *backend, gboolean load_packages) {
	do_poldek_destroy (backend);
	do_poldek_init (backend);

	if (load_packages)
		pb_load_packages (backend);
}

/**
 * backend_initalize:
 */
static void
backend_initalize (PkBackend *backend)
{
	pberror = g_new0 (PbError, 1);
	pberror->tslog = g_string_new ("");

	/* reference count for the global variables */
	if (ref++ > 1)
		return;

	do_poldek_init (backend);
}
/**
 * backend_destroy:
 */
static void
backend_destroy (PkBackend *backend)
{
	if (ref-- > 0)
		return;

	do_poldek_destroy (backend);

	/* release PbError struct */
	g_free (pberror->vfffmsg);
	g_string_free (pberror->tslog, TRUE);

	g_free (pberror);
}

/**
 * backend_download_packages:
 */
static gboolean
backend_download_packages_thread (PkBackend *backend)
{
	PercentageData *pd = pk_backend_get_pointer (backend, "percentage_ptr");
	struct poldek_ts *ts;
	struct vf_progress vf_progress;
	tn_array *pkgs;
	gchar **package_ids;
	const gchar *destdir;
	gint i;

	package_ids = pk_backend_get_strv (backend, "package_ids");
	destdir = pk_backend_get_string (backend, "directory");

	pkgs = n_array_new (10, (tn_fn_free)pkg_free, NULL);

	ts = poldek_ts_new (ctx, 0);

	setup_vf_progress (&vf_progress, backend);

	pb_load_packages (backend);

	for (i = 0; i < g_strv_length (package_ids); i++) {
		struct pkg *pkg = poldek_get_pkg_from_package_id (package_ids[i]);

		n_array_push (pkgs, pkg_link (pkg));

		pkg_free (pkg);
	}

	pd->bytesget = 0;
	pd->bytesdownload = poldek_get_bytes_to_download (ts, pkgs);

	if (!packages_fetch (poldek_get_pmctx (ts->ctx), pkgs, destdir, 1)) {
		/* something goes wrong */
	}

	poldek_ts_free (ts);

	poldek_backend_percentage_data_destroy (backend);

	pk_backend_finished (backend);

	return TRUE;
}

static void
backend_download_packages (PkBackend *backend, gchar **package_ids,
			   const gchar *directory)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_DOWNLOAD);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	poldek_backend_percentage_data_create (backend);
	pk_backend_thread_create (backend, backend_download_packages_thread);
}

/**
 * backend_get_groups:
 **/
static PkGroupEnum
backend_get_groups (PkBackend *backend)
{
	return (PK_GROUP_ENUM_ACCESSORIES |
		PK_GROUP_ENUM_ADMIN_TOOLS |
		PK_GROUP_ENUM_COMMUNICATION |
		PK_GROUP_ENUM_EDUCATION |
		PK_GROUP_ENUM_FONTS |
		PK_GROUP_ENUM_GAMES |
		PK_GROUP_ENUM_GRAPHICS |
		PK_GROUP_ENUM_LOCALIZATION |
		PK_GROUP_ENUM_MULTIMEDIA |
		PK_GROUP_ENUM_NETWORK |
		PK_GROUP_ENUM_OFFICE |
		PK_GROUP_ENUM_OTHER |
		PK_GROUP_ENUM_PROGRAMMING |
		PK_GROUP_ENUM_PUBLISHING |
		PK_GROUP_ENUM_SERVERS |
		PK_GROUP_ENUM_SYSTEM);
}

/**
 * backend_get_filters:
 */
static PkFilterEnum
backend_get_filters (PkBackend *backend)
{
	return (PK_FILTER_ENUM_NEWEST |
		PK_FILTER_ENUM_GUI |
		PK_FILTER_ENUM_INSTALLED |
		PK_FILTER_ENUM_DEVELOPMENT);
}

/**
 * backend_get_cancel:
 **/
static void
backend_get_cancel (PkBackend *backend)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_CANCEL);

	sigint_emit ();
}

/**
 * backend_get_depends:
 */
static gboolean
backend_get_depends_thread (PkBackend *backend)
{
	struct pkg	*pkg;
	tn_array	*deppkgs, *available, *installed;
	gint		i;
	gchar **package_ids;

	pb_load_packages (backend);

	deppkgs = n_array_new (2, NULL, NULL);

	installed = poldek_get_installed_packages ();
	available = poldek_get_avail_packages (ctx);
	package_ids = pk_backend_get_strv (backend, "package_ids");

	pkg = poldek_get_pkg_from_package_id (package_ids[0]);

	do_depends (installed, available, deppkgs, pkg, backend);

	n_array_sort_ex(deppkgs, (tn_fn_cmp)pkg_cmp_name_evr_rev);

	for (i = 0; i < n_array_size (deppkgs); i++) {
		struct pkg	*p = n_array_nth (deppkgs, i);

		poldek_backend_package (backend, p, PK_INFO_ENUM_UNKNOWN, pk_backend_get_uint (backend, "filters"));
	}

	pkg_free (pkg);

	n_array_free (deppkgs);
	n_array_free (available);
	n_array_free (installed);

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_get_depends (PkBackend *backend, PkFilterEnum filters, gchar **package_ids, gboolean recursive)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	pk_backend_thread_create (backend, backend_get_depends_thread);
}

/**
 * backend_get_details:
 */
static gboolean
backend_get_details_thread (PkBackend *backend)
{
	gchar **package_ids;
	struct pkg	*pkg = NULL;

	package_ids = pk_backend_get_strv (backend, "package_ids");

	pb_load_packages (backend);

	pkg = poldek_get_pkg_from_package_id (package_ids[0]);

	if (pkg) {
		struct pkguinf	*pkgu = NULL;
		PkGroupEnum	group;

		pkgu = pkg_uinf (pkg);

		group = pld_group_to_enum (pkg_group (pkg));

		if (pkgu) {
			pk_backend_details (backend,
						package_ids[0],
						pkguinf_get (pkgu, PKGUINF_LICENSE),
						group,
						pkguinf_get (pkgu, PKGUINF_DESCRIPTION),
						pkguinf_get (pkgu, PKGUINF_URL),
						pkg->fsize);
			pkguinf_free (pkgu);
		} else {
			pk_backend_details (backend,
						package_ids[0],
						"",
						group,
						"",
						"",
						pkg->fsize);
		}

		pkg_free (pkg);
	}

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_get_details (PkBackend *backend, gchar **package_ids)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	pk_backend_thread_create (backend, backend_get_details_thread);
}

/**
 * backend_get_files:
 */
static gboolean
backend_get_files_thread (PkBackend *backend)
{
	gchar **package_ids;
	struct pkg	*pkg;

	package_ids = pk_backend_get_strv (backend, "package_ids");

	pb_load_packages (backend);

	pkg = poldek_get_pkg_from_package_id (package_ids[0]);

	if (pkg) {
		struct pkgflist		*flist = pkg_get_flist (pkg);
		GString			*filelist;
		gchar			*result, *sep;
		gint			i, j;

		sep = "";

		if (!flist) {
			pkg_free (pkg);
			pk_backend_finished (backend);
			return TRUE;
		}

		filelist = g_string_new ("");

		for (i = 0; i < n_tuple_size (flist->fl); i++) {
			struct pkgfl_ent	*flent = n_tuple_nth (flist->fl, i);
			gchar			*dirname;

			dirname = g_strdup_printf ("%s%s", *flent->dirname == '/' ? "" : "/", flent->dirname);

			for (j = 0; j < flent->items; j++) {
				struct flfile	*f = flent->files[j];

				if (strcmp (dirname, "/") == 0)
					g_string_append_printf (filelist, "%s/%s", sep, f->basename);
				else
					g_string_append_printf (filelist, "%s%s/%s", sep, dirname, f->basename);

				sep = ";";
			}
			g_free (dirname);
		}

		result = g_string_free (filelist, FALSE);

		pk_backend_files (backend, package_ids[0], result);

		g_free (result);

		pkg_free (pkg);
	}

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_get_files (PkBackend *backend, gchar **package_ids)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	pk_backend_thread_create (backend, backend_get_files_thread);
}

/**
 * backend_get_packages:
 **/
static void
backend_get_packages (PkBackend *backend, PkFilterEnum filters)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_NONE);
	pk_backend_thread_create (backend, search_package_thread);
}

/**
 * backend_get_requires:
 */
static gboolean
backend_get_requires_thread (PkBackend *backend)
{
	struct pkg	*pkg;
	tn_array	*reqpkgs, *available, *installed;
	gint		i;
	gchar **package_ids;

	pb_load_packages (backend);

	reqpkgs = n_array_new (2, NULL, NULL);

	package_ids = pk_backend_get_strv (backend, "package_ids");
	pkg = poldek_get_pkg_from_package_id (package_ids[0]);
	installed = poldek_get_installed_packages ();
	available = poldek_get_avail_packages (ctx);

	do_requires (installed, available, reqpkgs, pkg, backend);

	/* sort output */
	n_array_sort_ex(reqpkgs, (tn_fn_cmp)pkg_cmp_name_evr_rev);

	for (i = 0; i < n_array_size (reqpkgs); i++) {
		struct pkg	*p = n_array_nth (reqpkgs, i);

		poldek_backend_package (backend, p, PK_INFO_ENUM_UNKNOWN, pk_backend_get_uint (backend, "filters"));
	}

	n_array_free (reqpkgs);
	n_array_free (installed);
	n_array_free (available);

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_get_requires (PkBackend	*backend, PkFilterEnum filters, gchar **package_ids, gboolean recursive)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	pk_backend_thread_create (backend, backend_get_requires_thread);
}

/**
 * backend_get_update_detail:
 */
static gchar*
get_obsoletedby_pkg (struct pkg *pkg)
{
	tn_array *dbpkgs;
	GString *obsoletes = NULL;
	gint i;

	g_return_val_if_fail (pkg != NULL, NULL);

	/* get installed packages */
	dbpkgs = poclidek_get_dent_packages (cctx, POCLIDEK_INSTALLEDDIR);

	if (dbpkgs == NULL)
		return NULL;

	for (i = 0; i < n_array_size (dbpkgs); i++) {
		struct pkg *dbpkg = n_array_nth (dbpkgs, i);

		if (pkg_caps_obsoletes_pkg_caps (pkg, dbpkg)) {
			gchar *package_id = package_id_from_pkg (dbpkg, "installed", 0);

			if (obsoletes) {
				obsoletes = g_string_append_c (obsoletes, '^');
				obsoletes = g_string_append (obsoletes, package_id);
			} else {
				obsoletes = g_string_new (package_id);
			}

			g_free (package_id);
		}
	}

	n_array_free (dbpkgs);

	return obsoletes ? g_string_free (obsoletes, FALSE) : NULL;
}

static gboolean
backend_get_update_detail_thread (PkBackend *backend)
{
	PkPackageId	*pi;
	gchar **package_ids;
	struct poclidek_rcmd	*rcmd;
	gchar		*command;

	package_ids = pk_backend_get_strv (backend, "package_ids");

	pb_load_packages (backend);

	pi = pk_package_id_new_from_string (package_ids[0]);

	rcmd = poclidek_rcmd_new (cctx, NULL);

	command = g_strdup_printf ("cd /installed; ls -q %s", pi->name);

	if (poclidek_rcmd_execline (rcmd, command)) {
		tn_array	*pkgs = NULL;
		struct pkg	*pkg = NULL, *upkg = NULL;

		pkgs = poclidek_rcmd_get_packages (rcmd);

		/* get one package */
		pkg = n_array_nth (pkgs, 0);

		if (strcmp (pkg->name, pi->name) == 0) {
			gchar	*updates, *obsoletes, *cve_url = NULL;
			tn_array *cves = NULL;

			updates = package_id_from_pkg (pkg, "installed", 0);

			upkg = poldek_get_pkg_from_package_id (package_ids[0]);

			obsoletes = get_obsoletedby_pkg (upkg);

			if ((cves = poldek_pkg_get_cves_from_pld_changelog (upkg, pkg->btime))) {
				GString	*string;
				gint i;

				string = g_string_new ("");

				for (i = 0; i < n_array_size (cves); i++) {
					gchar *cve = n_array_nth (cves, i);

					g_string_append_printf (string,
								"http://nvd.nist.gov/nvd.cfm?cvename=%s;%s",
								cve, cve);

					if ((i + 1) < n_array_size (cves))
						g_string_append_printf (string, ";");
				}

				cve_url = g_string_free (string, FALSE);
			}

			pk_backend_update_detail (backend,
						  package_ids[0],
						  updates,
						  obsoletes ? obsoletes : "",
						  "",
						  "",
						  cve_url ? cve_url : "",
						  PK_RESTART_ENUM_NONE,
						  "", NULL, PK_UPDATE_STATE_ENUM_UNKNOWN, NULL, NULL);

			g_free (updates);
			g_free (obsoletes);
			g_free (cve_url);

			n_array_cfree (&cves);
		}

		n_array_free (pkgs);
	} else {
		pk_backend_update_detail (backend,
					  package_ids[0],
					  "",
					  "",
					  "",
					  "",
					  "",
					  PK_RESTART_ENUM_NONE,
					  "");
	}

	g_free (command);
	poclidek_rcmd_free (rcmd);
	pk_package_id_free (pi);

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_get_update_detail (PkBackend *backend, gchar **package_ids)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	pk_backend_thread_create (backend, backend_get_update_detail_thread);
}

/**
 * backend_get_updates:
 */
static gboolean
backend_get_updates_thread (PkBackend *backend)
{
	struct poclidek_rcmd	*rcmd = NULL;

	pb_load_packages (backend);

	rcmd = poclidek_rcmd_new (cctx, NULL);

	if (rcmd) {
		if (poclidek_rcmd_execline (rcmd, "cd /all-avail; ls -q -u")) {
			tn_array	*pkgs = NULL;
			tn_array	*secupgrades = NULL;
			gint		i;

			pkgs = poclidek_rcmd_get_packages (rcmd);

			/* GetUpdates returns only the newest packages */
			do_newest (pkgs);

			secupgrades = poldek_get_security_updates ();

			for (i = 0; i < n_array_size (pkgs); i++) {
				struct pkg	*pkg = n_array_nth (pkgs, i);

				if (sigint_reached ())
					break;

				/* mark held packages as blocked */
				if (pkg->flags & PKG_HELD)
					poldek_backend_package (backend, pkg, PK_INFO_ENUM_BLOCKED, PK_FILTER_ENUM_NONE);
				else if (poldek_pkg_in_array (pkg, secupgrades, (tn_fn_cmp)pkg_cmp_name_evr))
					poldek_backend_package (backend, pkg, PK_INFO_ENUM_SECURITY, PK_FILTER_ENUM_NONE);
				else
					poldek_backend_package (backend, pkg, PK_INFO_ENUM_NORMAL, PK_FILTER_ENUM_NONE);
			}
			n_array_cfree (&secupgrades);
			n_array_free (pkgs);
		}
	}

	poclidek_rcmd_free (rcmd);

	if (sigint_reached ())
		pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_CANCELLED, "Action cancelled.");

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_get_updates (PkBackend *backend, PkFilterEnum filters)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	pk_backend_thread_create (backend, backend_get_updates_thread);
}

/**
 * backend_install_packages:
 */
static gboolean
backend_install_packages_thread (PkBackend *backend)
{
	struct poldek_ts	*ts;
	struct poclidek_rcmd	*rcmd;
	gchar			*command;
	struct vf_progress	vf_progress;
	gchar **package_ids;
	GString *cmd;
	gint i;

	pk_backend_set_uint (backend, "ts_type", TS_TYPE_ENUM_INSTALL);
	package_ids = pk_backend_get_strv (backend, "package_ids");

	setup_vf_progress (&vf_progress, backend);

	pb_load_packages (backend);

	ts = poldek_ts_new (ctx, 0);
	rcmd = poclidek_rcmd_new (cctx, ts);

	ts->setop(ts, POLDEK_OP_PARTICLE, 0);

	cmd = g_string_new ("install ");

	/* prepare command */
	for (i = 0; i < g_strv_length (package_ids); i++) {
		gchar	*nvra = poldek_get_nvra_from_package_id (package_ids[i]);

		g_string_append_printf (cmd, "%s ", nvra);

		g_free (nvra);
	}

	command = g_string_free (cmd, FALSE);

	pk_backend_set_status (backend, PK_STATUS_ENUM_DEP_RESOLVE);

	if (!poclidek_rcmd_execline (rcmd, command))
		pb_error_show (backend, PK_ERROR_ENUM_TRANSACTION_ERROR);
	else
		pb_error_check (backend);

	g_free (command);

	poldek_ts_free (ts);
	poclidek_rcmd_free (rcmd);

	poldek_backend_percentage_data_destroy (backend);

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_install_packages (PkBackend *backend, gchar **package_ids)
{
	if (!pk_backend_is_online (backend)) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot install package when offline!");
		pk_backend_finished (backend);
		return;
	}

	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	poldek_backend_percentage_data_create (backend);
	pk_backend_thread_create (backend, backend_install_packages_thread);
}

/**
 * FIXME: force currently omited
 * backend_refresh_cache:
 */
static gboolean
backend_refresh_cache_thread (PkBackend *backend)
{
	tn_array		*sources = NULL;
	struct vf_progress	vfpro;
	PercentageData *pd = pk_backend_get_pointer (backend, "percentage_ptr");

	setup_vf_progress (&vfpro, backend);

	pk_backend_set_percentage (backend, 1);

	sources = poldek_get_sources (ctx);

	if (sources) {
		gint	i;

		pk_backend_set_uint (backend, "ts_type", TS_TYPE_ENUM_REFRESH_CACHE);
		pd->step = 0;
		pd->nsources = 0;

		for (i = 0; i < n_array_size (sources); i++) {
			struct source	*src = n_array_nth (sources, i);

			if (src->flags & PKGSOURCE_NOAUTOUP)
				continue;
			else
				pd->nsources++;
		}

		for (i = 0; i < n_array_size (sources); i++) {
			struct source	*src = n_array_nth (sources, i);

			if (src->flags & PKGSOURCE_NOAUTOUP)
				continue;

			if (sigint_reached ())
				break;

			source_update (src, 0);
			pd->step++;
		}
		n_array_free (sources);
	}

	poldek_reload (backend, TRUE);

	pk_backend_set_percentage (backend, 100);

	poldek_backend_percentage_data_destroy (backend);

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_refresh_cache (PkBackend *backend, gboolean force)
{
	if (!pk_backend_is_online (backend)) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot refresh cache when offline!");
		pk_backend_finished (backend);
		return;
	}

	pk_backend_set_status (backend, PK_STATUS_ENUM_REFRESH_CACHE);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	poldek_backend_percentage_data_create (backend);
	pk_backend_thread_create (backend, backend_refresh_cache_thread);
}

/**
 * backend_remove_packages:
 */
static gboolean
backend_remove_packages_thread (PkBackend *backend)
{
	struct poclidek_rcmd	*rcmd;
	struct poldek_ts	*ts;
	GString *cmd;
	gchar *command;
	gchar **package_ids;
	gint i;

	package_ids = pk_backend_get_strv (backend, "package_ids");
	pb_load_packages (backend);

	ts = poldek_ts_new (ctx, 0);
	rcmd = poclidek_rcmd_new (cctx, ts);

	cmd = g_string_new ("uninstall ");

	/* prepare command */
	for (i = 0; i < g_strv_length (package_ids); i++) {
		gchar	*nvra = poldek_get_nvra_from_package_id (package_ids[i]);

		g_string_append_printf (cmd, "%s ", nvra);

		g_free (nvra);
	}

	command = g_string_free (cmd, FALSE);

	pk_backend_set_status (backend, PK_STATUS_ENUM_DEP_RESOLVE);

	if (!poclidek_rcmd_execline (rcmd, command))
	{
		pk_backend_error_code (backend, PK_ERROR_ENUM_CANNOT_REMOVE_SYSTEM_PACKAGE, pberror->tslog->str);
	}

	g_free (command);

	poldek_ts_free (ts);
	poclidek_rcmd_free (rcmd);

	pk_backend_finished (backend);
	return TRUE;
}

static void
backend_remove_packages (PkBackend *backend, gchar **package_ids, gboolean allow_deps, gboolean autoremove)
{
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();
	pk_backend_thread_create (backend, backend_remove_packages_thread);
}

/**
 * backend_resolve:
 */
static void
backend_resolve (PkBackend *backend, PkFilterEnum filters, gchar **package_ids)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);

	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_RESOLVE);
	pk_backend_thread_create (backend, search_package_thread);
}

/**
 * backend_search_details:
 */
static void
backend_search_details (PkBackend *backend, PkFilterEnum filters, const gchar *search)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();
	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_DETAILS);
	pk_backend_thread_create (backend, search_package_thread);
}

/**
 * backend_search_file:
 */
static void
backend_search_file (PkBackend *backend, PkFilterEnum filters, const gchar *search)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();
	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_FILE);
	pk_backend_thread_create (backend, search_package_thread);
}

/**
 * backend_search_group:
 */
static void
backend_search_group (PkBackend *backend, PkFilterEnum filters, const gchar *search)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();
	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_GROUP);
	pk_backend_thread_create (backend, search_package_thread);
}

/**
 * backend_search_name:
 */
static void
backend_search_name (PkBackend *backend, PkFilterEnum filters, const gchar *search)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();
	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_NAME);
	pk_backend_thread_create (backend, search_package_thread);
}

/**
 * backend_update_packages:
 */
static void
backend_update_packages (PkBackend *backend, gchar **package_ids)
{
	if (!pk_backend_is_online (backend)) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot update packages when offline!");
		pk_backend_finished (backend);
		return;
	}

	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	poldek_backend_percentage_data_create (backend);
	pk_backend_set_uint (backend, "ts_type", TS_TYPE_ENUM_UPDATE);
	pk_backend_thread_create (backend, update_packages_thread);
}

/**
 * backend_update_system:
 **/
static void
backend_update_system (PkBackend *backend)
{
	if (!pk_backend_is_online (backend)) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot update system when offline!");
		pk_backend_finished (backend);
		return;
	}

	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	poldek_backend_percentage_data_create (backend);
	pk_backend_set_uint (backend, "ts_type", TS_TYPE_ENUM_UPDATE);
	pk_backend_set_bool (backend, "update_system", TRUE);
	pk_backend_thread_create (backend, update_packages_thread);
}

/**
 * backend_get_repo_list:
 */
static void
backend_get_repo_list (PkBackend *backend, PkFilterEnum filters)
{
	tn_array	*sources = NULL;

	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, FALSE, TRUE);
	pb_error_clean ();

	sources = poldek_get_sources (ctx);

	if (sources) {
		gint	i;

		for (i = 0; i < n_array_size (sources); i++) {
			struct source	*src = n_array_nth (sources, i);
			gboolean	enabled = TRUE;

			if (src->flags & PKGSOURCE_NOAUTO)
				enabled = FALSE;

			pk_backend_repo_detail (backend, src->name, src->path, enabled);
		}

		n_array_free (sources);
	}

	pk_backend_finished (backend);
}

/**
 * backend_what_provides:
 **/
static void
backend_what_provides (PkBackend *backend, PkFilterEnum filters, PkProvidesEnum provides, const gchar *search)
{
	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);
	poldek_backend_set_allow_cancel (backend, TRUE, TRUE);
	pb_error_clean ();

	pk_backend_set_uint (backend, "mode", SEARCH_ENUM_PROVIDES);
	pk_backend_thread_create (backend, search_package_thread);
}

PK_BACKEND_OPTIONS (
	"poldek",					/* description */
	"Marcin Banasiak <megabajt@pld-linux.org>",	/* author */
	backend_initalize,				/* initalize */
	backend_destroy,				/* destroy */
	backend_get_groups,				/* get_groups */
	backend_get_filters,				/* get_filters */
	backend_get_cancel,				/* cancel */
	backend_download_packages,			/* download_packages */
	backend_get_depends,				/* get_depends */
	backend_get_details,				/* get_details */
	NULL,						/* get_distro_upgrades */
	backend_get_files,				/* get_files */
	backend_get_packages,				/* get_packages */
	backend_get_repo_list,				/* get_repo_list */
	backend_get_requires,				/* get_requires */
	backend_get_update_detail,			/* get_update_detail */
	backend_get_updates,				/* get_updates */
	NULL,						/* install_files */
	backend_install_packages,			/* install_packages */
	NULL,						/* install_signature */
	backend_refresh_cache,				/* refresh_cache */
	backend_remove_packages,			/* remove_packages */
	NULL,						/* repo_enable */
	NULL,						/* repo_set_data */
	backend_resolve,				/* resolve */
	NULL,						/* rollback */
	backend_search_details,				/* search_details */
	backend_search_file,				/* search_file */
	backend_search_group,				/* search_group */
	backend_search_name,				/* search_name */
	NULL,						/* service pack */
	backend_update_packages,			/* update_packages */
	backend_update_system,				/* update_system */
	backend_what_provides				/* what_provides */
);

