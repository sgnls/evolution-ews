/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* camel-ews-utils.c
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include "server/camel-ews-settings.h"
#include "server/e-ews-message.h"

#include "camel-ews-utils.h"

#define SUBFOLDER_DIR_NAME     "subfolders"
#define SUBFOLDER_DIR_NAME_LEN 10

/**
 * e_path_to_physical:
 * @prefix: a prefix to prepend to the path, or %NULL
 * @path: the virtual path to convert to a filesystem path.
 *
 * This converts the "virtual" path @path into an expanded form that
 * allows a given name to refer to both a file and a directory. The
 * expanded path will have a "subfolders" directory inserted between
 * each path component. If the path ends with "/", the returned
 * physical path will end with "/subfolders"
 *
 * If @prefix is non-%NULL, it will be prepended to the returned path.
 *
 * Returns: the expanded path
 **/
gchar *
e_path_to_physical (const gchar *prefix,
                    const gchar *vpath)
{
	const gchar *p, *newp;
	gchar *dp;
	gchar *ppath;
	gint ppath_len;
	gint prefix_len;

	while (*vpath == '/')
		vpath++;
	if (!prefix)
		prefix = "";

	/* Calculate the length of the real path. */
	ppath_len = strlen (vpath);
	ppath_len++;	/* For the ending zero.  */

	prefix_len = strlen (prefix);
	ppath_len += prefix_len;
	ppath_len++;	/* For the separating slash.  */

	/* Take account of the fact that we need to translate every
	 * separator into 'subfolders/'.
	 */
	p = vpath;
	while (1) {
		newp = strchr (p, '/');
		if (newp == NULL)
			break;

		ppath_len += SUBFOLDER_DIR_NAME_LEN;
		ppath_len++; /* For the separating slash.  */

		/* Skip consecutive slashes.  */
		while (*newp == '/')
			newp++;

		p = newp;
	};

	ppath = g_malloc (ppath_len);
	dp = ppath;

	memcpy (dp, prefix, prefix_len);
	dp += prefix_len;
	*(dp++) = '/';

	/* Copy the mangled path.  */
	p = vpath;
	while (1) {
		newp = strchr (p, '/');
		if (newp == NULL) {
			strcpy (dp, p);
			break;
		}

		memcpy (dp, p, newp - p + 1); /* '+ 1' to copy the slash too.  */
		dp += newp - p + 1;

		memcpy (dp, SUBFOLDER_DIR_NAME, SUBFOLDER_DIR_NAME_LEN);
		dp += SUBFOLDER_DIR_NAME_LEN;

		*(dp++) = '/';

		/* Skip consecutive slashes.  */
		while (*newp == '/')
			newp++;

		p = newp;
	}

	return ppath;
}

static gboolean
find_folders_recursive (const gchar *physical_path,
                        const gchar *path,
                        EPathFindFoldersCallback callback,
                        gpointer data)
{
	GDir *dir;
	gchar *subfolder_directory_path;
	gboolean ok;

	if (*path) {
		if (!callback (physical_path, path, data))
			return FALSE;

		subfolder_directory_path = g_strdup_printf ("%s/%s", physical_path, SUBFOLDER_DIR_NAME);
	} else {
		/* On the top level, we have no folders and,
		 * consequently, no subfolder directory.
		 */

		subfolder_directory_path = g_strdup (physical_path);
	}

	/* Now scan the subfolders and load them. */
	dir = g_dir_open (subfolder_directory_path, 0, NULL);
	if (dir == NULL) {
		g_free (subfolder_directory_path);
		return TRUE;
	}

	ok = TRUE;
	while (ok) {
		struct stat file_stat;
		const gchar *dirent;
		gchar *file_path;
		gchar *new_path;

		dirent = g_dir_read_name (dir);
		if (dirent == NULL)
			break;

		file_path = g_strdup_printf ("%s/%s", subfolder_directory_path, dirent);

		if (g_stat (file_path, &file_stat) < 0 ||
		    !S_ISDIR (file_stat.st_mode)) {
			g_free (file_path);
			continue;
		}

		new_path = g_strdup_printf ("%s/%s", path, dirent);

		ok = find_folders_recursive (file_path, new_path, callback, data);

		g_free (file_path);
		g_free (new_path);
	}

	g_dir_close (dir);
	g_free (subfolder_directory_path);

	return ok;
}

/**
 * e_path_find_folders:
 * @prefix: directory to start from
 * @callback: Callback to invoke on each folder
 * @data: Data for @callback
 *
 * Walks the folder tree starting at @prefix and calls @callback
 * on each folder.
 *
 * Returns: %TRUE on success, %FALSE if an error occurs at any point
 **/
gboolean
e_path_find_folders (const gchar *prefix,
                     EPathFindFoldersCallback callback,
                     gpointer data)
{
	return find_folders_recursive (prefix, "", callback, data);
}

/**
 * e_path_rmdir:
 * @prefix: a prefix to prepend to the path, or %NULL
 * @path: the virtual path to convert to a filesystem path.
 *
 * This removes the directory pointed to by @prefix and @path
 * and attempts to remove its parent "subfolders" directory too
 * if it's empty.
 *
 * Returns: -1 (with errno set) if it failed to rmdir the
 * specified directory. 0 otherwise, whether or not it removed
 * the parent directory.
 **/
gint
e_path_rmdir (const gchar *prefix,
              const gchar *vpath)
{
	gchar *physical_path, *p;

	/* Remove the directory itself */
	physical_path = e_path_to_physical (prefix, vpath);
	if (g_rmdir (physical_path) == -1) {
		g_free (physical_path);
		return -1;
	}

	/* Attempt to remove its parent "subfolders" directory,
	 * ignoring errors since it might not be empty.
	 */

	p = strrchr (physical_path, '/');
	if (p[1] == '\0') {
		g_free (physical_path);
		return 0;
	}
	*p = '\0';
	p = strrchr (physical_path, '/');
	if (!p || strcmp (p + 1, SUBFOLDER_DIR_NAME) != 0) {
		g_free (physical_path);
		return 0;
	}

	g_rmdir (physical_path);
	g_free (physical_path);
	return 0;
}

void
do_flags_diff (flags_diff_t *diff,
               guint32 old,
               guint32 _new)
{
	diff->changed = old ^ _new;
	diff->bits = _new & diff->changed;
}

gchar *
ews_concat (const gchar *prefix,
            const gchar *suffix)
{
	gsize len;

	len = strlen (prefix);
	if (len == 0 || prefix[len - 1] == '/')
		return g_strdup_printf ("%s%s", prefix, suffix);
	else
		return g_strdup_printf ("%s%c%s", prefix, '/', suffix);
}

void
strip_lt_gt (gchar **string,
             gint s_offset,
             gint e_offset)
{
	gchar *temp = NULL;
	gint len;

	temp = g_strdup (*string);
	len = strlen (*string);

	*string = (gchar *)g_malloc0 (len-1);
	*string = memcpy(*string, temp+s_offset, len-e_offset);
	g_free (temp);
}

CamelFolderInfo *
camel_ews_utils_build_folder_info (CamelEwsStore *store,
                                   const gchar *fid)
{
	CamelEwsStoreSummary *ews_summary = store->summary;
	CamelFolderInfo *fi;

	fi = camel_folder_info_new ();
	fi->full_name = camel_ews_store_summary_get_folder_full_name (
		ews_summary, fid, NULL);
	fi->display_name = camel_ews_store_summary_get_folder_name (
		ews_summary, fid, NULL);
	fi->flags = camel_ews_store_summary_get_folder_flags (
		ews_summary, fid, NULL);
	fi->unread = camel_ews_store_summary_get_folder_unread (
		ews_summary, fid, NULL);
	fi->total = camel_ews_store_summary_get_folder_total (
		ews_summary, fid, NULL);

	return fi;
}

static void
sync_deleted_folders (CamelEwsStore *store,
                      GSList *deleted_folders)
{
	CamelEwsStoreSummary *ews_summary = store->summary;
	GSList *l;

	for (l = deleted_folders; l != NULL; l = g_slist_next (l)) {
		const gchar *fid = l->data;
		EEwsFolderType ftype;
		CamelFolderInfo *fi;
		GError *error = NULL;

		if (!camel_ews_store_summary_has_folder (ews_summary, fid))
			continue;

		ftype = camel_ews_store_summary_get_folder_type (ews_summary, fid, NULL);
		if (ftype == E_EWS_FOLDER_TYPE_MAILBOX) {
			fi = camel_ews_utils_build_folder_info (store, fid);

			camel_ews_store_summary_remove_folder (ews_summary, fid, &error);
			camel_store_folder_deleted ((CamelStore *) store, fi);

			g_clear_error (&error);
		}
	}
}

static gboolean
ews_utils_rename_folder (CamelEwsStore *store,
                         EEwsFolderType ftype,
                         const gchar *fid,
                         const gchar *changekey,
                         const gchar *pfid,
                         const gchar *display_name,
                         const gchar *old_fname,
                         GError **error)
{
	CamelEwsStoreSummary *ews_summary = store->summary;
	CamelFolderInfo *fi;

	camel_ews_store_summary_set_change_key (ews_summary, fid, changekey);
	if (display_name)
		camel_ews_store_summary_set_folder_name (ews_summary, fid, display_name);
	if (pfid)
		camel_ews_store_summary_set_parent_folder_id (ews_summary, fid, pfid);

	if (ftype == E_EWS_FOLDER_TYPE_MAILBOX) {
		fi = camel_ews_utils_build_folder_info (store, fid);
		camel_store_folder_renamed ((CamelStore *) store, old_fname, fi);
	}

	return TRUE;
}

static void
sync_updated_folders (CamelEwsStore *store,
                      GSList *updated_folders)
{
	CamelEwsStoreSummary *ews_summary = store->summary;
	GSList *l;

	for (l = updated_folders; l != NULL; l = g_slist_next (l)) {
		EEwsFolder *ews_folder = (EEwsFolder *)	l->data;
		EEwsFolderType ftype;
		gchar *folder_name;
		gchar *display_name;
		const EwsFolderId *fid, *pfid;

		ftype = e_ews_folder_get_folder_type (ews_folder);
		if (ftype != E_EWS_FOLDER_TYPE_MAILBOX)
			continue;

		fid = e_ews_folder_get_id (ews_folder);
		folder_name = camel_ews_store_summary_get_folder_full_name (ews_summary, fid->id, NULL);

		pfid = e_ews_folder_get_parent_id (ews_folder);
		display_name = g_strdup (e_ews_folder_get_name (ews_folder));

		/* If the folder is moved or renamed (which are separate
		 * operations in Exchange, unfortunately, then the name
		 * or parent folder will change. Handle both... */
		if (pfid || display_name) {
			GError *error = NULL;
			gchar *new_fname = NULL;

			if (pfid) {
				gchar *pfname;

				/* If the display name wasn't changed, its basename is still
				 * the same as it was before... */
				if (!display_name)
					display_name = camel_ews_store_summary_get_folder_name (
						ews_summary, fid->id, NULL);
				if (!display_name)
					goto done;

				pfname = camel_ews_store_summary_get_folder_full_name (ews_summary, pfid->id, NULL);

				/* If the lookup failed, it'll be because the new parent folder
				 * is the message folder root. */
				if (pfname) {
					new_fname = g_strconcat (pfname, "/", display_name, NULL);
					g_free (pfname);
				} else
					new_fname = g_strdup (display_name);
			} else {
				/* Parent folder not changed; just basename */
				const gchar *last_slash;

				/* Append new display_name to old parent directory name... */
				last_slash = g_strrstr (folder_name, "/");
				if (last_slash)
					new_fname = g_strdup_printf (
						"%.*s/%s",
						(gint)(last_slash - folder_name),
						folder_name, display_name);
				else /* ...unless it was a child of the root folder */
					new_fname = g_strdup (display_name);
			}

			if (strcmp (new_fname, folder_name))
				ews_utils_rename_folder (
					store, ftype,
					fid->id, fid->change_key,
					pfid ? pfid->id : NULL,
					display_name, folder_name, &error);
			g_free (new_fname);
			g_clear_error (&error);
		}
 done:
		g_free (folder_name);
		g_free (display_name);
	}
}

/* FIXME get the real folder ids of the system folders using
 * by fetching them using distinguished folder ids once */
static void
add_folder_to_summary (CamelEwsStore *store,
                       EEwsFolder *folder)
{
	CamelEwsStoreSummary *ews_summary = store->summary;
	const EwsFolderId *pfid, *fid;
	const gchar *dname;
	gint64 unread, total, ftype;

	fid = e_ews_folder_get_id (folder);
	pfid = e_ews_folder_get_parent_id (folder);
	dname = e_ews_folder_get_name (folder);
	total = e_ews_folder_get_total_count (folder);
	unread = e_ews_folder_get_unread_count (folder);
	ftype = e_ews_folder_get_folder_type (folder);

	camel_ews_store_summary_new_folder (
		ews_summary, fid->id,
		pfid->id, fid->change_key,
		dname, ftype, 0, total);
	camel_ews_store_summary_set_folder_unread (ews_summary, fid->id, unread);
}

static void
sync_created_folders (CamelEwsStore *ews_store,
                      GSList *created_folders)
{
	GSList *l;

	for (l = created_folders; l != NULL; l = g_slist_next (l)) {
		EEwsFolder *folder = (EEwsFolder *) l->data;
		EEwsFolderType ftype;
		CamelFolderInfo *fi;
		const EwsFolderId *fid;

		ftype = e_ews_folder_get_folder_type (folder);
		if (ftype != E_EWS_FOLDER_TYPE_MAILBOX)
			continue;

		fid = e_ews_folder_get_id (folder);

		/* FIXME: Sort folders so that a child is always added *after*
		 * its parent. But since the old code was already completely
		 * broken and would just go into an endless loop if the server
		 * didn't return the folders in the 'right' order for that,
		 * let's worry about that in a later commit. */
		add_folder_to_summary (ews_store, folder);

		if (ftype == E_EWS_FOLDER_TYPE_MAILBOX) {
			fi = camel_ews_utils_build_folder_info (ews_store, fid->id);
			camel_store_folder_created ((CamelStore *) ews_store, fi);
			camel_folder_info_free (fi);
		}
	}
}

void
ews_utils_sync_folders (CamelEwsStore *ews_store,
                        GSList *created_folders,
                        GSList *deleted_folders,
                        GSList *updated_folders)
{
	GError *error = NULL;

	sync_deleted_folders (ews_store, deleted_folders);
	sync_updated_folders (ews_store, updated_folders);
	sync_created_folders (ews_store, created_folders);

	camel_ews_store_summary_save (ews_store->summary, &error);
	if (error != NULL) {
		g_print ("Error while saving store summary %s \n", error->message);
		g_clear_error (&error);
	}
	return;
}

void
camel_ews_utils_sync_deleted_items (CamelEwsFolder *ews_folder,
                                    GSList *items_deleted)
{
	CamelFolder *folder;
	const gchar *full_name;
	CamelFolderChangeInfo *ci;
	CamelEwsStore *ews_store;
	GSList *l;
	GList *items_deleted_list = NULL;

	ci = camel_folder_change_info_new ();
	ews_store = (CamelEwsStore *) camel_folder_get_parent_store ((CamelFolder *) ews_folder);

	folder = (CamelFolder *) ews_folder;
	full_name = camel_folder_get_full_name (folder);

	for (l = items_deleted; l != NULL; l = g_slist_next (l)) {
		const gchar *id = l->data;

		items_deleted_list = g_list_prepend (items_deleted_list, (gpointer) id);

		camel_folder_summary_remove_uid (folder->summary, id);
		camel_folder_change_info_remove_uid (ci, id);
	}

	items_deleted_list = g_list_reverse (items_deleted_list);
	camel_db_delete_uids (((CamelStore *) ews_store)->cdb_w, full_name, items_deleted_list, NULL);
	g_list_free (items_deleted_list);

	if (camel_folder_change_info_changed (ci)) {
		camel_folder_summary_touch (folder->summary);
		camel_folder_summary_save_to_db (folder->summary, NULL);
		camel_folder_changed (folder, ci);
	}
	camel_folder_change_info_free (ci);

	g_slist_foreach (items_deleted, (GFunc) g_free, NULL);
	g_slist_free (items_deleted);
}

static const gchar *
ews_utils_rename_label (const gchar *cat,
                        gint from_cat)
{
	gint i;

	/* this is a mapping from Exchange/Outlook categories to
	 * evolution labels based on the standard colours */
	const gchar *labels[] = {
		"Red Category", "$Labelimportant",
		"Orange Category", "$Labelwork",
		"Green Category", "$Labelpersonal",
		"Blue Category", "$Labeltodo",
		"Purple Category", "$Labellater",
		NULL, NULL
	};

	if (!cat || !*cat)
		return "";

	for (i = 0; labels[i]; i += 2) {
		if (from_cat) {
			if (!g_ascii_strcasecmp (cat, labels[i]))
				return labels[i + 1];
		} else {
			if (!g_ascii_strcasecmp (cat, labels[i + 1]))
				return labels[i];
		}
	}
	return cat;
}

void
ews_utils_replace_server_user_flags (ESoapMessage *msg,
                                     CamelEwsMessageInfo *mi)
{
	const CamelFlag *flag;

	/* transfer camel flags to become the categories as an XML
	 * array of strings */
	for (flag = camel_message_info_user_flags (&mi->info); flag;
	     flag = flag->next) {
		const gchar *n = ews_utils_rename_label (flag->name, 0);
		if (*n == '\0')
			continue;
		/* This is a mismatch between evolution flags and
		 * exchange categories.  Evolution uses a
		 * receipt-handled flag for message receipts, which we
		 * don't want showing up in the categories, so
		 * silently drop it here */
		if (strcmp (n, "receipt-handled") == 0)
			continue;
		e_ews_message_write_string_parameter (msg, "String", NULL, n);
	}
}

static void
ews_utils_merge_server_user_flags (EEwsItem *item,
                                   CamelEwsMessageInfo *mi)
{
	GSList *list = NULL;
	const GSList *p;
	const CamelFlag *flag;

	/* transfer camel flags to a list */
	for (flag = camel_message_info_user_flags (&mi->info); flag;
	     flag = flag->next)
		list = g_slist_append (list, (gchar *) flag->name);

	/* we're transferring from server only, so just dump them */
	for (p = list; p; p = p->next) {
		camel_flag_set (&mi->info.user_flags, p->data, 0);
	}
	//g_slist_foreach (list, (GFunc) g_free, NULL);
	g_slist_free (list);

	/* now transfer over all the categories */
	for (p = e_ews_item_get_categories (item); p; p = p->next) {
		camel_flag_set (
			&mi->info.user_flags,
			ews_utils_rename_label (p->data, 1), 1);
	}
}

static gint
ews_utils_get_server_flags (EEwsItem *item)
{
	gboolean flag;
	EwsImportance importance;
	gint server_flags = 0;

	e_ews_item_is_read (item, &flag);
	if (flag)
		server_flags |= CAMEL_MESSAGE_SEEN;
	else
		server_flags &= ~CAMEL_MESSAGE_SEEN;

	e_ews_item_is_forwarded (item, &flag);
	if (flag)
		server_flags |= CAMEL_MESSAGE_FORWARDED;
	else
		server_flags &= ~CAMEL_MESSAGE_FORWARDED;

	e_ews_item_is_answered (item, &flag);
	if (flag)
		server_flags |= CAMEL_MESSAGE_ANSWERED;
	else
		server_flags &= ~CAMEL_MESSAGE_ANSWERED;

	importance = e_ews_item_get_importance (item);
	if (importance == EWS_ITEM_HIGH)
		server_flags |= CAMEL_MESSAGE_FLAGGED;

	/* TODO Update replied flags */

	return server_flags;
}

static const gchar *
form_email_string_from_mb (EEwsConnection *cnc,
                           const EwsMailbox *mb,
                           GCancellable *cancellable)
{
	if (mb) {
		GString *str;
		gchar *email = NULL;

		if (g_strcmp0 (mb->routing_type, "EX") == 0) {
			e_ews_connection_ex_to_smtp_sync (
				cnc, EWS_PRIORITY_MEDIUM,
				mb->name, mb->email, &email,
				cancellable, NULL);

			/* do not scare users with EX addresses */
			if (!email)
				email = g_strdup ("");
		}

		str = g_string_new ("");
		if (mb->name && mb->name[0]) {
			g_string_append (str, mb->name);
			g_string_append (str, " ");
		}

		if (mb->email || email) {
			g_string_append (str, "<");
			g_string_append (str, email ? email : mb->email);
			g_string_append (str, ">");
		}

		g_free (email);

		return camel_pstring_add (g_string_free (str, FALSE), TRUE);
	} else
	       return camel_pstring_strdup ("");
}

static const gchar *
form_recipient_list (EEwsConnection *cnc,
                     const GSList *recipients,
                     GCancellable *cancellable)
{
	const GSList *l;
	GString *str = NULL;
	const gchar *ret;

	if (!recipients)
		return NULL;

	for (l = recipients; l != NULL; l = g_slist_next (l)) {
		EwsMailbox *mb = (EwsMailbox *) l->data;
		const gchar *mb_str = form_email_string_from_mb (cnc, mb, cancellable);

		if (!str)
			str = g_string_new ("");
		else
			str = g_string_append (str, ", ");

		str = g_string_append (str, mb_str);
	}

	ret = camel_pstring_add (str->str, TRUE);
	g_string_free (str, FALSE);

	return ret;
}

static guint8 *
get_md5_digest (const guchar *str)
{
	guint8 *digest;
	gsize length;
	GChecksum *checksum;

	length = g_checksum_type_get_length (G_CHECKSUM_MD5);
	digest = g_malloc0 (length);

	checksum = g_checksum_new (G_CHECKSUM_MD5);
	g_checksum_update (checksum, str, -1);
	g_checksum_get_digest (checksum, digest, &length);
	g_checksum_free (checksum);

	return digest;
}

static void
ews_set_threading_data (CamelEwsMessageInfo *mi,
                        EEwsItem *item)
{
	const gchar *references, *inreplyto;
	gint count = 0;
	const gchar *message_id;
	struct _camel_header_references *refs, *irt, *scan;
	guint8 *digest;
	gchar *msgid;

	/* set message id */
	message_id = e_ews_item_get_msg_id (item);
	msgid = camel_header_msgid_decode (message_id);
	if (msgid) {
		digest = get_md5_digest ((const guchar *) msgid);
		memcpy (mi->info.message_id.id.hash, digest, sizeof (mi->info.message_id.id.hash));
		g_free (digest);
		g_free (msgid);
	}

	/* Process References: header */
	references = e_ews_item_get_references (item);
	refs = camel_header_references_decode (references);

	/* Prepend In-Reply-To: contents to References: for summary info */
	inreplyto = e_ews_item_get_in_replyto (item);
	irt = camel_header_references_inreplyto_decode (inreplyto);
	if (irt) {
		irt->next = refs;
		refs = irt;
	}
	if (!refs)
		return;

	count = camel_header_references_list_size (&refs);
	mi->info.references = g_malloc (sizeof (*mi->info.references) + ((count - 1) * sizeof (mi->info.references->references[0])));
	scan = refs;
	count = 0;

	while (scan) {
		digest = get_md5_digest ((const guchar *) scan->id);
		memcpy (mi->info.references->references[count].id.hash, digest, sizeof (mi->info.message_id.id.hash));
		g_free (digest);

		count++;
		scan = scan->next;
	}

	mi->info.references->size = count;
	camel_header_references_list_clear (&refs);
}

void
camel_ews_utils_sync_updated_items (CamelEwsFolder *ews_folder,
                                    GSList *items_updated)
{
	CamelFolder *folder;
	CamelFolderChangeInfo *ci;
	GSList *l;

	ci = camel_folder_change_info_new ();
	folder = (CamelFolder *) ews_folder;

	for (l = items_updated; l != NULL; l = g_slist_next (l)) {
		EEwsItem *item = (EEwsItem *) l->data;
		const EwsId *id;
		CamelEwsMessageInfo *mi;

		if (e_ews_item_get_item_type (item) == E_EWS_ITEM_TYPE_ERROR) {
			g_object_unref (item);
			continue;
		}

		id = e_ews_item_get_id (item);
		mi = (CamelEwsMessageInfo *) camel_folder_summary_get (folder->summary, id->id);
		if (mi) {
			gint server_flags;

			server_flags = ews_utils_get_server_flags (item);
			ews_utils_merge_server_user_flags (item, mi);
			if (camel_ews_update_message_info_flags (
				folder->summary, (CamelMessageInfo *) mi,
				server_flags, NULL))
				camel_folder_change_info_change_uid (ci, mi->info.uid);

			g_free (mi->change_key);
			mi->change_key = g_strdup (id->change_key);
			mi->info.dirty = TRUE;

			camel_message_info_free (mi);
			g_object_unref (item);
			continue;
		}

		g_object_unref (item);
	}

	if (camel_folder_change_info_changed (ci)) {
		camel_folder_summary_touch (folder->summary);
		camel_folder_summary_save_to_db (folder->summary, NULL);
		camel_folder_changed ((CamelFolder *) ews_folder, ci);
	}
	camel_folder_change_info_free (ci);
	g_slist_free (items_updated);
}

void
camel_ews_utils_sync_created_items (CamelEwsFolder *ews_folder,
                                    EEwsConnection *cnc,
                                    GSList *items_created)
{
	CamelFolder *folder;
	CamelFolderChangeInfo *ci;
	GSList *l;

	if (!items_created)
		return;

	ci = camel_folder_change_info_new ();
	folder = (CamelFolder *) ews_folder;

	for (l = items_created; l != NULL; l = g_slist_next (l)) {
		EEwsItem *item = (EEwsItem *) l->data;
		CamelEwsMessageInfo *mi;
		const EwsId *id;
		const EwsMailbox *from;
		EEwsItemType item_type;
		const GSList *to, *cc;
		gboolean has_attachments;
		guint32 server_flags;

		if (!item)
			continue;

		if (e_ews_item_get_item_type (item) == E_EWS_ITEM_TYPE_ERROR) {
			g_object_unref (item);
			continue;
		}

		id = e_ews_item_get_id (item);
		mi = (CamelEwsMessageInfo *) camel_folder_summary_get (folder->summary, id->id);
		if (mi) {
			camel_message_info_free (mi);
			g_object_unref (item);
			continue;
		}

		mi = (CamelEwsMessageInfo *) camel_message_info_new (folder->summary);

		if (mi->info.content == NULL) {
			mi->info.content = camel_folder_summary_content_info_new (folder->summary);
			mi->info.content->type = camel_content_type_new ("multipart", "mixed");
		}

		item_type = e_ews_item_get_item_type (item);
		if (item_type == E_EWS_ITEM_TYPE_CALENDAR_ITEM ||
			 item_type == E_EWS_ITEM_TYPE_MEETING_MESSAGE ||
			 item_type == E_EWS_ITEM_TYPE_MEETING_REQUEST ||
			 item_type == E_EWS_ITEM_TYPE_MEETING_RESPONSE ||
			 item_type == E_EWS_ITEM_TYPE_MEETING_RESPONSE)
			camel_message_info_set_user_flag ((CamelMessageInfo *) mi, "$has_cal", TRUE);

		mi->info.uid = camel_pstring_strdup (id->id);
		mi->info.size = e_ews_item_get_size (item);
		mi->info.subject = camel_pstring_strdup (e_ews_item_get_subject (item));
		mi->item_type = item_type;
		mi->change_key = g_strdup (id->change_key);

		mi->info.date_sent = e_ews_item_get_date_sent (item);
		mi->info.date_received = e_ews_item_get_date_received (item);

		from = e_ews_item_get_from (item);
		if (!from)
			from = e_ews_item_get_sender (item);
		mi->info.from = form_email_string_from_mb (cnc, from, NULL);

		to = e_ews_item_get_to_recipients (item);
		mi->info.to = form_recipient_list (cnc, to, NULL);

		cc = e_ews_item_get_cc_recipients (item);
		mi->info.cc = form_recipient_list (cnc, cc, NULL);

		e_ews_item_has_attachments (item, &has_attachments);
		if (has_attachments)
			mi->info.flags |= CAMEL_MESSAGE_ATTACHMENTS;

		ews_set_threading_data (mi, item);
		server_flags = ews_utils_get_server_flags (item);
		ews_utils_merge_server_user_flags (item, mi);

		mi->info.flags |= server_flags;
		mi->server_flags = server_flags;

		camel_folder_summary_add (folder->summary, (CamelMessageInfo *) mi);

		/* camel_folder_summary_add() sets folder_flagged flag
		 * on the message info, but this is a fresh item downloaded
		 * from the server, thus unset it, to avoid resync up to the server
		 * on folder leave/store
		*/
		mi->info.flags &= ~CAMEL_MESSAGE_FOLDER_FLAGGED;

		camel_folder_change_info_add_uid (ci, id->id);
		camel_folder_change_info_recent_uid (ci, id->id);

		g_object_unref (item);
	}

	if (camel_folder_change_info_changed (ci)) {
		camel_folder_summary_touch (folder->summary);
		camel_folder_summary_save_to_db (folder->summary, NULL);
		camel_folder_changed ((CamelFolder *) ews_folder, ci);
	}
	camel_folder_change_info_free (ci);
	g_slist_free (items_created);
}
