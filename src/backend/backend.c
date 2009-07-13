/*
    4store - a clustered RDF storage and query engine

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 *  Copyright (C) 2006 Steve Harris for Garlik
 */

#include <stdlib.h>
#include <stdarg.h>
#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>

#include "common/error.h"
#include "common/params.h"
#include "common/timing.h"
#include "backend.h"
#include "import-backend.h"
#include "backend-intl.h"
#include "sort.h"
#include "lock.h"
#include "mhash.h"
#include "tlist.h"

/* used to indicate to backend processes that they need to reopen thier
 * index files */
static volatile int need_reload = 0;

int fs_backend_need_reload()
{
    if (need_reload) {
	need_reload = 0;
	return 1;
    }

    return 0;
}

static void do_sigusr2(int sig)
{
    need_reload = 1;
}

fs_backend *fs_backend_init(const char *db_name, int flags)
{
    fs_backend *ret = calloc(1, sizeof(fs_backend));
    ret->db_name = db_name;
    ret->segment = -1;
    if (flags & FS_BACKEND_NO_OPEN) {
	return ret;
    }

    ret->md = fs_metadata_open(db_name);
    if (!ret->md) {
	fs_error(LOG_CRIT, "cannot read metadata file for kb %s", db_name);

	return NULL;
    }

    if (!fs_metadata_get_string(ret->md, FS_MD_NAME, NULL)) {
	fs_error(LOG_ERR, "no value for KB name in metadata, does KB exist?");

	return NULL;
    }

    const char *hashfunc = fs_metadata_get_string(ret->md, FS_MD_HASHFUNC, "MD5");
    if (strcmp(hashfunc, FS_HASH)) {
	fs_error(LOG_ERR, "stored hash function does not match server's hash function");
	fs_error(LOG_ERR, "rebuild code with correct function or replace store");

	return NULL;
    }

    const char *store_type = fs_metadata_get_string(ret->md, FS_MD_STORE, "semi-native");
    if (strcmp(store_type, "native")) {
	fs_error(LOG_ERR, "tried to open %s store with native backend", store_type);

	return NULL;
    }

    if (strcmp(fs_metadata_get_string(ret->md, FS_MD_NAME, "-no-match-"), db_name)) {
	fs_error(LOG_CRIT, "metadata and opened KB name don't match %s / %s", db_name, fs_metadata_get_string(ret->md, FS_MD_NAME, "-no-match-"));

	return NULL;
    }

    ret->segments = fs_metadata_get_int(ret->md, FS_MD_SEGMENTS, 0);
    const int version = fs_metadata_get_int(ret->md, FS_MD_VERSION, 0);
    if (version == -1) { 
	fs_error(LOG_CRIT, "cannot find number of segments in KB %s", db_name);

	return NULL;
    }
    if (version > FS_CURRENT_TABLE_VERSION ||
        version < FS_EARLIEST_TABLE_VERSION) {
	fs_error(LOG_ERR, "wrong table metadata version in KB %s", db_name);

	return NULL;
    }

    ret->salt = fs_metadata_get_int(ret->md, FS_MD_SALT, 0);
    ret->hash = g_strdup(fs_metadata_get_string(ret->md, FS_MD_HASH, ""));
    ret->model_data = fs_metadata_get_bool(ret->md, FS_MD_MODEL_DATA, 0);
    ret->model_dirs = fs_metadata_get_bool(ret->md, FS_MD_MODEL_DIRS, 0);
    ret->model_files = fs_metadata_get_bool(ret->md, FS_MD_MODEL_FILES, 0);

    ret->transaction = -1;

    /* preload indexes for primary segments */
    if (flags & FS_BACKEND_PRELOAD) {
	fs_rid_vector *segs = fs_metadata_get_int_vector(ret->md, FS_MD_SEGMENT_P);
	for (int i=0; i<segs->length; i++) {
            ret->segment = segs->data[i];
        }
        fs_rid_vector_free(segs);
	ret->segment = -1;
    } else {
	struct sigaction reload_action = {
	  .sa_handler = &do_sigusr2,
	  .sa_flags = (SA_RESTART),
	};
	sigemptyset(&reload_action.sa_mask);
	sigaction(SIGUSR2, &reload_action, NULL);
    }

    return ret;
}

const char *fs_backend_get_kb(fs_backend *be)
{
    return be->db_name;
}

int fs_backend_get_segments(fs_backend *be)
{
    return be->segments;
}

fs_segment fs_backend_get_segment(fs_backend *be)
{
    return be->segment;
}

void fs_backend_fini(fs_backend *be)
{
    if (!be) return;
  
    fs_backend_cleanup_files(be); 
    fs_backend_close_files(be, be->segment);
    fs_metadata_close(be->md);
    g_free((void *)be->hash);
    free(be);
}

void fs_bnode_alloc(fs_backend *be, int count, fs_rid *from, fs_rid *to)
{
    *from = fs_metadata_get_int(be->md, FS_MD_BNODE, 1);
    *to = (*from) + count;
    char *newval = g_strdup_printf("%lld", *to);
    fs_metadata_set(be->md, FS_MD_BNODE, newval);
    fs_metadata_flush(be->md);
    g_free(newval);
}

int fs_segments(fs_backend *be, int *segments)
{
    int count = 0;
    fs_rid_vector *segs = fs_metadata_get_int_vector(be->md, FS_MD_SEGMENT_P);
    for (int i=0; i < segs->length; i++) {
	segments[i] = segs->data[i];
    }
    count += segs->length;
    fs_rid_vector_free(segs);
    segs = fs_metadata_get_int_vector(be->md, FS_MD_SEGMENT_M);
    for (int i=0; i < segs->length; i++) {
	segments[count + i] = segs->data[i];
    }
    count += segs->length;
    fs_rid_vector_free(segs);

    return count;
}

void fs_node_segments(fs_backend *be, char *segments)
{
    for (int k = 0; k < be->segments; ++k) {
	segments[k] = '\0'; /* not available */
    }

    fs_rid_vector *segs = fs_metadata_get_int_vector(be->md, FS_MD_SEGMENT_P);
    for (int i=0; i < segs->length; i++) {
	fs_segment seg = segs->data[i];
	segments[seg] = 'p';
    }
    fs_rid_vector_free(segs);
    segs = fs_metadata_get_int_vector(be->md, FS_MD_SEGMENT_M);
    for (int i=0; i < segs->length; i++) {
	fs_segment seg = segs->data[i];
	segments[seg] = 'm';
    }
    fs_rid_vector_free(segs);
}

fs_import_timing fs_get_import_times(fs_backend *be, int seg)
{
    if (seg < 0 || seg >= be->segments) {
	fs_error(LOG_ERR, "segment number out of range");

	return be->in_time[0];
    }

    return be->in_time[seg];
}

fs_query_timing fs_get_query_times(fs_backend *be, int seg)
{
    if (seg < 0 || seg >= be->segments) {
	fs_error(LOG_ERR, "segment number out of range");

	return be->out_time[0];
    }

    return be->out_time[seg];
}

void fs_backend_set_min_free(fs_backend *be, float min_free)
{
    be->min_free = min_free;
}

int fs_start_import(fs_backend *be, int seg)
{
    int errs = 0;

    /* TODO update metadata ? */

    return errs;
}

static int fs_commit(fs_backend *be, fs_segment seg, int force_trans)
{
    fs_rid_set *rs = NULL;

    if (fs_list_length(be->pending_delete) > 0) {
	rs = fs_rid_set_new();
	fs_rid val;
	fs_list_rewind(be->pending_delete);
	while (fs_list_next_value(be->pending_delete, &val)) {
	    fs_rid_set_add(rs, val);
	}
    }

    if (be->pended_import) {
	/* push out pending data */

	fs_rid quad[4];
	for (int i=0; i<be->ptree_length; i++) {
	    if (be->ptrees[i].pend) {
		/* TODO we might want to sort here, to improve locality and
		 * uniq */

		/* insert into s tree */
		fs_list_flush(be->ptrees[i].pend);
		fs_list_rewind(be->ptrees[i].pend);
		while (fs_list_next_value(be->ptrees[i].pend, quad)) {
		    fs_rid pair[2] = { quad[0], quad[3] };
		    fs_ptree_add(be->ptrees[i].ptree_s, quad[1], pair, 0);
		}

		/* insert into o tree */
		fs_list_rewind(be->ptrees[i].pend);
		while (fs_list_next_value(be->ptrees[i].pend, quad)) {
		    fs_rid pair[2] = { quad[0], quad[1] };
		    fs_ptree_add(be->ptrees[i].ptree_o, quad[3], pair, 0);
		}

		/* this is potentially expensive, we could just truncate the list
		 * files here */
		fs_list_unlink(be->ptrees[i].pend);
		be->ptrees[i].pend = NULL;
	    }
	}

	be->pended_import = 0;
    }

    if (rs) {
	fs_rid_set_free(rs);
    }

    return 0;
}

int fs_stop_import(fs_backend *be, int seg)
{
    double then = fs_time();

    if (fs_backend_is_transaction_open(be)) {
	/* were in a transaction, don't need to do anything else */
	/* NB transactions not supported in this branch */

	return 0;
    }

    fs_rhash_flush(be->res);
    if (be->models) {
	fs_mhash_flush(be->models);
    }

    int ret = fs_commit(be, seg, 0);
    double now = fs_time();
    be->in_time[seg].rebuild += now - then;

    /* TODO update metadata? */

    return ret;
}

int fs_backend_transaction(fs_backend *be, fs_segment seg, int op)
{
    fs_error(LOG_CRIT, "transactions not supported in this branch");

    return 1;
}

int fs_backend_model_get_usage(fs_backend *be, int seg, fs_rid model, fs_index_node *val)
{
    if (!be->models) {
	fs_error(LOG_ERR, "model hash not open");
    }

    int ret = fs_mhash_get(be->models, model, val);

    return ret;
}

int fs_backend_model_set_usage(fs_backend *be, int seg, fs_rid model, fs_index_node val)
{
    int ret = 0;

    if (fs_backend_is_transaction_open(be)) {
	if (val) {
	    ret = fs_list_add(be->pending_insert, &model);
	    fs_list_flush(be->pending_insert);
	} else {
	    fs_error(LOG_CRIT, "tried to set model usage to false in transaction");
	    ret = 1;
	}
    } else {
	fs_index_node mval = 0;
	if (fs_mhash_get(be->models, model, &mval)) {
	    fs_error(LOG_ERR, "fs_mhash_get on %016llx failed", model);
	}
	if (mval != val) {
	    ret = fs_mhash_put(be->models, model, val);
	    if (val == 1) {
		fs_tlist *tl = fs_tlist_open(be, model, O_CREAT | O_RDWR);
		if (tl) {
		    fs_tlist_close(tl);
		} else {
		    fs_error(LOG_CRIT, "Failed to create model data file for "
			     "%016llx", model);
		}
	    }
	}
    }

    return ret;
}

fs_ptree *fs_backend_get_ptree(fs_backend *be, fs_rid pred, int object)
{
    for (int t=0; t<be->ptree_length; t++) {
	if (pred == be->ptrees[t].pred) {
	    if (object == 0) return be->ptrees[t].ptree_s;
	    else return be->ptrees[t].ptree_o;
	}
    }

    return NULL;
}

int fs_backend_open_ptree(fs_backend *be, fs_rid pred, int flags)
{
    if (be->ptree_length == be->ptree_size) {
	be->ptree_size *= 2;
	be->ptrees = realloc(be->ptrees, be->ptree_size * sizeof(struct ptree_ref));
	if (!be->ptrees) {
	    fs_error(LOG_CRIT, "realloc failed");
	}
    }

    be->ptrees[be->ptree_length].pred = pred;
    be->ptrees[be->ptree_length].ptree_s =
		    fs_ptree_open(be, pred, 's', flags | O_RDWR, be->pairs);
    be->ptrees[be->ptree_length].ptree_o =
		    fs_ptree_open(be, pred, 'o', flags | O_RDWR, be->pairs);
    be->ptrees[be->ptree_length].pend = NULL;
    be->approx_size += fs_ptree_count(be->ptrees[be->ptree_length].ptree_s);

    return (be->ptree_length)++;
}

int fs_backend_open_files_intl(fs_backend *be, fs_segment seg, int flags, int files, char *file, int line)
{
    if (!be) {
	fs_error(LOG_CRIT, "tried to open NULL backend");

	return 1;
    }
    if (be->segment != -1) {
	if (be->segment != seg) {
	    fs_error_intl(LOG_CRIT, file, line, NULL, "tried to reopen backend files with different segment, was %d, now %d", be->segment, seg);

	    return 1;
	}
	fs_error_intl(LOG_CRIT, file, line, NULL, "reopening backend files for segment %d", seg);
	fs_backend_close_files(be, seg);
    }

    be->segment = seg;
    if (!be->checked_transaction) {
	be->transaction = fs_lock_taken(be, "trans");
	be->checked_transaction = 1;
    }

    if (files & FS_OPEN_LEX && !be->res) {
        be->res = fs_rhash_open(be, "res", flags);
        if (!be->res) {
            fs_error(LOG_ERR, "failed to open resurce file");

            return 1;
        }
    }

    if (files & FS_OPEN_MHASH && !be->models) {
	be->models = fs_mhash_open(be, "models", flags);
	if (!be->models) {
	    fs_error(LOG_ERR, "failed to open model hash");

	    return 1;
	}
	be->model_list = fs_tbchain_open(be, "mlist", flags);
	if (!be->model_list) {
	    fs_error(LOG_ERR, "failed to open model list");

	    return 1;
	}
    }

    if (!be->predicates) {
	be->predicates = fs_list_open(be, "predicates", sizeof(fs_rid), flags);
	fs_rid pred;
	int length = fs_list_length(be->predicates);
	be->ptree_length = 0;
	if (length < 16) {
	    be->ptree_size = 16;
	} else {
	    be->ptree_size = length;
	}
	be->ptrees = calloc(be->ptree_size, sizeof(struct ptree_ref));
	fs_list_rewind(be->predicates);
	be->pairs = fs_ptable_open(be, "pairs", flags | O_RDWR);
	if (!be->pairs) {
	    fs_error(LOG_CRIT, "failed to open ptable file");

	    return 1;
	}
	while (fs_list_next_value(be->predicates, &pred)) {
	    fs_backend_open_ptree(be, pred, flags);
	}
    }

    if (files & FS_OPEN_DEL) {
	be->pending_delete = fs_list_open(be, "del", sizeof(fs_rid), flags);
	be->pending_insert = fs_list_open(be, "ins", sizeof(fs_rid), flags);
    }

    return 0;
}

int fs_backend_cleanup_files(fs_backend *be)
{
    for (int i=0; i<be->ptree_length; i++) {
	if (be->ptrees[i].pend) {
	    fs_list_unlink(be->ptrees[i].pend);
	    be->ptrees[i].pend = NULL;
	}
    }

    return 0;
}

int fs_backend_unlink_indexes(fs_backend *be, fs_segment seg)
{
    if (be->segment == -1) {
	return 1;
    }

    if (seg != be->segment) {
	fs_error(LOG_ERR, "tried to unlink files from different backend: %d not %d", seg, be->segment);

	return 1;
    }

    for (int i=0; i<be->ptree_length; i++) {
	fs_ptree_unlink(be->ptrees[i].ptree_s);
	fs_ptree_close(be->ptrees[i].ptree_s);
	fs_ptree_unlink(be->ptrees[i].ptree_o);
	fs_ptree_close(be->ptrees[i].ptree_o);
	be->ptrees[i].pred = 0LL;
	be->ptrees[i].ptree_s = NULL;
	be->ptrees[i].ptree_o = NULL;
    }

    be->ptree_length = 0;

    fs_rid_vector *models = fs_mhash_get_keys(be->models);
    for (int i=0; i<models->length; i++) {
	fs_index_node val;
	fs_mhash_get(be->models, models->data[i], &val);
	if (val == 1) {
	    fs_tlist *tl = fs_tlist_open(be, models->data[i], O_RDWR);
	    fs_tlist_unlink(tl);
	    fs_tlist_close(tl);
	}
    }
    fs_rid_vector_free(models);

    if (be->pairs) {
	fs_ptable_unlink(be->pairs);
	fs_ptable_close(be->pairs);
	be->pairs = NULL;
    }

    if (be->model_list) {
	fs_tbchain_unlink(be->model_list);
	fs_tbchain_close(be->model_list);
	be->model_list = NULL;
    }

    /* TODO remove TList support or cleaner impl. here */
    char *command = g_strdup_printf("rm -f "FS_TLIST_ALL, be->db_name, be->segment);
    system(command);
    g_free(command);

    return 0;
}

int fs_backend_close_files(fs_backend *be, fs_segment seg)
{
    if (be->segment == -1) {
	return 1;
    }

    if (seg != be->segment) {
	fs_error(LOG_ERR, "tried to close files from different backend: %d not %d", seg, be->segment);

	return 1;
    }

    if (be->lex_f) {
	fclose(be->lex_f);
	be->lex_f = NULL;
    }
    if (be->res) {
	fs_rhash_close(be->res);
	be->res = NULL;
    }
    if (be->models) {
	fs_mhash_close(be->models);
	be->models = NULL;
    }
    if (be->model_list) {
	fs_tbchain_close(be->model_list);
	be->model_list = NULL;
    }
    if (be->pending_delete) {
	fs_list_close(be->pending_delete);
	be->pending_delete = NULL;
    }
    if (be->pending_insert) {
	fs_list_close(be->pending_insert);
	be->pending_insert = NULL;
    }
    for (int i=0; i<be->ptree_length; i++) {
	fs_ptree_close(be->ptrees[i].ptree_s);
	fs_ptree_close(be->ptrees[i].ptree_o);
	be->ptrees[i].pred = 0LL;
	be->ptrees[i].ptree_s = NULL;
	be->ptrees[i].ptree_o = NULL;
    }
    free(be->ptrees);
    be->ptrees = NULL;
    be->ptree_length = 0;
    be->ptree_size = 0;
    if (be->predicates) {
	fs_list_close(be->predicates);
    }
    be->predicates = NULL;
    be->segment = -1;

    return 0;
}

int fs_backend_is_transaction_open_intl(fs_backend *be, char *file, int line)
{
    if (be->transaction == -1) {
	fs_error_intl(LOG_CRIT, file, line, NULL,
		"tried to read if transaction was open before it was set");

	return 0;
    }

    return be->transaction;
}

int fs_backend_model_dirs(fs_backend *be)
{
    return be->model_dirs;
}

int fs_backend_model_files(fs_backend *be)
{
    return be->model_files;
}

/* vi:set ts=8 sts=4 sw=4: */