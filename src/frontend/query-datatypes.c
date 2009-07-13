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
#include <glib.h>
#include <string.h>

#include "query-datatypes.h"
#include "query-intl.h"
#include "common/error.h"

//#define DEBUG_MERGE 1
//#define DEBUG_COMPARE 1
//#define DEBUG_BINDING "ac"

fs_binding *fs_binding_new()
{
    fs_binding *b = calloc(FS_BINDING_MAX_VARS+1, sizeof(fs_binding));
    for (int i=0; i<FS_BINDING_MAX_VARS; i++) {
	b[i].appears = -1;
	b[i].depends = -1;
    }

    return b;
}

void fs_binding_free(fs_binding *b)
{
    if (!b) return;

    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	g_free(b[i].name);
        b[i].name = NULL;
	fs_rid_vector_free(b[i].vals);
        b[i].vals = NULL;
	fs_rid_vector_free(b[i].ubs);
        b[i].ubs = NULL;
    }
    memset(b, 0, sizeof(fs_binding));
    free(b);
}

int fs_binding_set_expression(fs_binding *b, const char *name, rasqal_expression *ex)
{
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
            b[i].expression = ex;

            return 0;
        }
    }

    return 1;
}

int fs_binding_any_bound(fs_binding *b)
{
    for (int i=0; b[i].name; i++) {
	if (b[i].bound) {
	    return 1;
	}
    }

    return 0;
}

int fs_binding_bound_intersects(fs_query *q, int block, fs_binding *b, rasqal_literal *l[4])
{
    for (int i=0; b[i].name; i++) {
	if (b[i].bound) {
	    for (int j=0; j<4; j++) {
		if (l[j] && l[j]->type == RASQAL_LITERAL_VARIABLE &&
		    !strcmp((char *)l[j]->value.variable->name, b[i].name)) {
		    /* if this var is bound only in this union block, and its
		     * not yet be used in this branch, then we dont iterate */
		    if (q->union_group[block] > 0 &&
			q->union_group[block] == q->union_group[b[i].appears] &&
			b[i].bound_in_block[block] == 0) {
			continue;
		    }
		    return 1;
		}
	    }
	}
    }

    return 0;
}

int fs_binding_length(fs_binding *b)
{
    int length = 0;
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	if (b[i].vals && b[i].vals->length > length) {
	    length = b[i].vals->length;
	}
    }

    return length;
}

int fs_binding_width(fs_binding *b)
{
    int width;
    for (width=0; b[width].name; width++) ;

    return width;
}

fs_binding *fs_binding_add(fs_binding *b, const char *name, fs_rid val, int projected)
{
    int i;

#ifdef DEBUG_BINDING
    if (strcmp(DEBUG_BINDING, name)) printf("@@ add("DEBUG_BINDING", %016llx, %d)\n", val, projected);
#endif
    for (i=0; 1; i++) {
	if (b[i].name == NULL) {
	    if (i == FS_BINDING_MAX_VARS) {
		fs_error(LOG_ERR, "variable limit of %d exceeded",
			FS_BINDING_MAX_VARS);

		return NULL;
	    }
	    b[i].name = g_strdup(name);
	    if (val != FS_RID_NULL) {
                if (b[i].vals || b[i].ubs) {
                    fs_error(LOG_WARNING, "loosing pointer to rid_vector");
                }
		b[i].vals = fs_rid_vector_new_from_args(1, val);
		b[i].ubs = fs_rid_vector_new_from_args(1, 0);
		b[i].bound = 1;
	    } else {
                if (b[i].vals || b[i].ubs) {
                    fs_error(LOG_WARNING, "loosing pointer to rid_vector");
                }
		b[i].vals = fs_rid_vector_new(0);
		b[i].ubs = fs_rid_vector_new(0);
	    }
	    b[i].proj = projected;
	    b[i].need_val = projected;

	    return b+i;
	}
	if (!strcmp(b[i].name, name)) {
	    fs_rid_vector_append(b[i].vals, val);
	    fs_rid_vector_append(b[i].ubs, 0);
	    b[i].bound = 1;
	    b[i].proj |= projected;
	    b[i].need_val |= projected;

	    return b+i;
	}
    }

    return NULL;
}

void fs_binding_clear_vector(fs_binding *b, const char *name)
{
#ifdef DEBUG_BINDING
    if (strcmp(DEBUG_BINDING, name)) printf("@@ clear_vector("DEBUG_BINDING")\n");
#endif
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    b[i].vals->length = 0;
	    b[i].ubs->length = 0;
	    return;
	}
    }
}

fs_binding *fs_binding_copy_and_clear(fs_binding *b)
{
#ifdef DEBUG_BINDING
    printf("@@ copy_and_clear()\n");
#endif
    fs_binding *b2 = fs_binding_new();

    memcpy(b2, b, sizeof(fs_binding) * FS_BINDING_MAX_VARS);
    for (int i=0; 1; i++) {
        if (!b[i].name) {
            break;
        }
        b[i].name = g_strdup(b2[i].name);
        b[i].vals = fs_rid_vector_new(b2[i].vals->size);
        b[i].vals->length = 0;
        b[i].ubs = fs_rid_vector_new(b2[i].ubs->size);
        b[i].ubs->length = 0;

        /* at this point we can clear the bound flag as
           shortcuts on variables bound to the empty
           list are now handled by a wrapper round
           fsp_bind_*() and look to the parent code
           just like we had sent them to the backend */
        b[i].bound = 0;
    }

    return b2;
}

void fs_binding_clear(fs_binding *b)
{
#ifdef DEBUG_BINDING
    printf("@@ clear()\n");
#endif
    fs_binding *b2 = fs_binding_new();

    memcpy(b2, b, sizeof(fs_binding) * FS_BINDING_MAX_VARS);
    for (int i=0; 1; i++) {
        if (!b[i].name) {
            break;
        }
        b2[i].name = NULL;
        b[i].vals->length = 0;
        b[i].ubs->length = 0;
        b[i].bound = 0;
    }
}

void fs_binding_add_vector(fs_binding *b, const char *name, fs_rid_vector *vals)
{
#ifdef DEBUG_BINDING
    if (!strcmp(DEBUG_BINDING, name)) printf("@@ add_vector("DEBUG_BINDING", %p)\n", vals);
#endif
    int i;
    for (i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    for (int j=0; vals && j<vals->length; j++) {
		fs_rid_vector_append(b[i].vals, vals->data[j]);
	    }
	    b[i].bound = 1;
	    return;
	}
    }

    if (i == FS_BINDING_MAX_VARS) {
	fs_error(LOG_ERR, "variable limit (%d) exceeded",
		FS_BINDING_MAX_VARS);

	return;
    }

    /* name wasn't found, add it */
    if (!b[i].name) {
	b[i].name = g_strdup(name);
	b[i].vals = fs_rid_vector_copy(vals);
	b[i].bound = 1;
    }
}

fs_binding *fs_binding_get(fs_binding *b, const char *name)
{
#ifdef DEBUG_BINDING
    if (!strcmp(DEBUG_BINDING, name)) printf("@@ get("DEBUG_BINDING")\n");
#endif
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    return b+i;
	}
    }

    return NULL;
}

fs_rid fs_binding_get_val(fs_binding *b, const char *name, int idx, int *bound)
{
#ifdef DEBUG_BINDING
    if (!strcmp(DEBUG_BINDING, name)) printf("@@ get_val("DEBUG_BINDING", %d)\n", idx);
#endif
    int i;

    for (i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    if (bound) *bound = b[i].bound;
	    if (!*bound) {
		return FS_RID_NULL;
	    }
	    if (idx >= 0 && idx < b[i].vals->length) {
		return b[i].vals->data[idx];
	    }
	    fs_error(LOG_ERR, "val request out of range for variable '%s'", name);

	    return FS_RID_NULL;
	}
    }

    return FS_RID_NULL;
}

fs_rid_vector *fs_binding_get_vals(fs_binding *b, const char *name, int *bound)
{
#ifdef DEBUG_BINDING
    if (!strcmp(DEBUG_BINDING, name)) printf("@@ get_vals("DEBUG_BINDING")\n");
#endif
    int i;

    for (i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    if (bound) *bound = b[i].bound;
	    return b[i].vals;
	}
    }

    /*
    fs_error(LOG_ERR, "binding lookup on unknown variable '%s'", name);
    */

    return NULL;
}

void fs_binding_clear_used_all(fs_binding *b)
{
#ifdef DEBUG_BINDING
    printf("@@ clear_used_all()\n");
#endif
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	b[i].used = 0;
    }
}

void fs_binding_set_used(fs_binding *b, const char *name)
{
#ifdef DEBUG_BINDING
    if (!strcmp(DEBUG_BINDING, name)) printf("@@ set_used("DEBUG_BINDING")\n");
#endif
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    b[i].used = 1;
	    break;
	}
    }
}

int fs_binding_get_projected(fs_binding *b, const char *name)
{
#ifdef DEBUG_BINDING
    if (!strcmp(DEBUG_BINDING, name)) printf("@@ get_projected("DEBUG_BINDING")\n");
#endif
    for (int i=0; 1; i++) {
	if (!b[i].name) break;
	if (!strcmp(b[i].name, name)) {
	    return b[i].proj;
	}
    }

    /*
    fs_error(LOG_ERR, "binding lookup on unknown variable '%s'", name);
    */

    return 0;
}

void fs_binding_copy_row_unused(fs_binding *from, int row, int count, fs_binding *to)
{
    for (int i=0; 1; i++) {
	if (!from[i].name) break;
	if (from[i].used) {
	    continue;
	}
	fs_rid val;
	if (row < from[i].vals->length) {
	    val = from[i].vals->data[row];
	} else {
	    val = FS_RID_NULL;
	}
	for (int j=0; j<count; j++) {
	    fs_rid_vector_append(to[i].vals, val);
	}
    }
}

void fs_binding_print(fs_binding *b, FILE *out)
{
    int length = fs_binding_length(b);

    for (int c=0; b[c].name; c++) {
	if (b[c].bound) {
	    fprintf(out, " %20.20s", b[c].name);
	} else {
	    fprintf(out, " %13.13s", b[c].name);
	}
    }
    fprintf(out, "\n");
    for (int c=0; b[c].name; c++) {
	if (b[c].bound) {
	    fprintf(out, " u       %c%c%c%c A%02d D%02d",
		    b[c].proj ? 'p' : '-', b[c].used ? 'u' : '-',
		    b[c].need_val ? 'n' : '-', b[c].bound ? 'b' : '-',
		    b[c].appears, b[c].depends);
	} else {
	    fprintf(out, "   %c%c%c A%02d D%02d",
		    b[c].proj ? 'p' : '-', b[c].used ? 'u' : '-',
		    b[c].need_val ? 'n' : '-', 
		    b[c].appears, b[c].depends);
	}
    }
    fprintf(out, "\n");
    for (int r=0; r<length; r++) {
	for (int c=0; b[c].name; c++) {
	    if (b[c].bound) {
		if (r < b[c].vals->length && b[c].vals->data[r] == FS_RID_NULL) {
		    fprintf(out, " 0%19s", "null");
		} else {
		    fprintf(out, "%2x %18llx", r < b[c].ubs->length ?(int)(b[c].ubs->data[r]) : 0, r < b[c].vals->length ? b[c].vals->data[r] : -1);
		}
	    } else {
		fprintf(out, "%14s", "null");
	    }
	}
	fprintf(out, "\n");
	if (length > 25 && r > 20 && (length - r) > 2) {

	    fprintf(out, "...\n");
	    r = length - 3;
	}
    }
}

/* q should be set when joining */
static int binding_row_compare(fs_query *q, fs_binding *b1, fs_binding *b2, int p1, int p2, int length1, int length2, int flags)
{
    if (p1 >= length1) {
#ifdef DEBUG_COMPARE
        printf("CMP from past end\n");
#endif
	return 1;
    } else if (p2 >= length2) {
#ifdef DEBUG_COMPARE
        printf("CMP to past end\n");
#endif
	return -1;
    }

    for (int i=0; b1[i].name; i++) {
	if (!b1[i].sort) continue;

        const fs_rid b1v = p1 < b1[i].vals->length ? b1[i].vals->data[p1] : FS_RID_NULL;
        const fs_rid b2v = p2 < b2[i].vals->length ? b2[i].vals->data[p2] : FS_RID_NULL;

	/* this looks a bit odd, but if one of the values is NULL then we can
	 * regard it as a match when we're joining (ie. q is set) */
	if (q && !(flags & FS_BIND_OPTIONAL) &&
		 (b1v == FS_RID_NULL || b2v == FS_RID_NULL)) {
	    continue;
	}

	if (b1v > b2v) {
#ifdef DEBUG_COMPARE
            printf("CMP %llx > %llx\n", b1v, b2v);
#endif
	    return 1;
	}
	if (b1v < b2v) {
#ifdef DEBUG_COMPARE
            printf("CMP %llx < %llx\n", b1[i].vals->data[p1], b2[i].vals->data[p2]);
#endif
	    return -1;
	}
	/* if the variables have been bound in different union blocks then they
	 * don't match */
	int u1 = 0, u2 = 0;
        if (b1[i].ubs && b1[i].ubs->length > p1) u1 = b1[i].ubs->data[p1];
        if (b2[i].ubs && b2[i].ubs->length > p2) u2 = b2[i].ubs->data[p2];
	if (u1 != u2 && q && q->union_group[u1] == q->union_group[u2]) {
#ifdef DEBUG_COMPARE
            printf("CMP union different %llx != %llx\n", b1[i].ubs->data[p1], b2[i].ubs->data[p2]);
#endif
	    return 2;
	}
    }

    return 0;
}

static int binding_data_compare(fs_binding *b, int p1, fs_rid w[])
{
    for (int i=0; b[i].name; i++) {
	if (!b[i].sort) continue;
        fs_rid val = p1 < b[i].vals->length ? b[i].vals->data[p1] : FS_RID_NULL;
	if (val > w[i]) {
	    return 1;
	}
	if (val < w[i]) {
	    return -1;
	}
    }

    return 0;
}

static inline fs_rid binding_get_value(fs_binding *b, int col, int row)
{
    if (row >= b[col].vals->length) {
        return FS_RID_NULL;
    }

    return b[col].vals->data[row];
}

static inline void binding_set_value(fs_binding *b, int col, int row, fs_rid value)
{
    if (row >= b[col].vals->length) {
        for (int i=b[col].vals->length; i<row; i++) {
            fs_rid_vector_append(b[col].vals, FS_RID_NULL);
        }
    }
    b[col].vals->data[row] = value;
}

static void binding_swap(fs_binding *b, int x, int y)
{
    for (int i=0; b[i].name; i++) {
	if (!b[i].bound) continue;
	fs_rid tmp = binding_get_value(b, i, x);
	binding_set_value(b, i, x, binding_get_value(b, i, y));
	binding_set_value(b, i, y, tmp);
    }
}

static int binding_partition(fs_binding *b, int left, int right)
{
    int pivot = (left + right) / 2;
    int count = fs_binding_width(b);
    fs_rid pvals[count];
    for (int i=0; i<count; i++) {
	if (b[i].sort && pivot < b[i].vals->length) {
	    pvals[i] = b[i].vals->data[pivot];
	} else {
	    pvals[i] = FS_RID_NULL;
	}
    }

    binding_swap(b, pivot, right);

    int store = left;

    for (int p = left; p < right; p++) {
	if (binding_data_compare(b, p, pvals) <= 0) {
	    binding_swap(b, store, p);
	    store++;
	}
    }
    binding_swap(b, right, store);

    return store;
}

static void fs_binding_sort_intl(fs_binding *b, int left, int right)
{
    if (right > left) {
	int pivot = binding_partition(b, left, right);
	fs_binding_sort_intl(b, left, pivot-1);
	fs_binding_sort_intl(b, pivot+1, right);
    }
}

/* inplace quicksort on an array of rid_vectors */
void fs_binding_sort(fs_binding *b)
{
    int scount = 0;
    int length = fs_binding_length(b);

    for (int i=0; b[i].name; i++) {
	if (b[i].sort) scount++;
        if (b[i].vals->length < length) {
            for (int j=b[i].vals->length; j<length; j++) {
                fs_rid_vector_append(b[i].vals, FS_RID_NULL);
            }
        }
#if 0
        if (b[i].vals->length == 0 && length > 0) {
            for (int j=0; j<length; j++) {
                fs_rid_vector_append(b[i].vals, FS_RID_NULL);
            }
        } else if (b[i].vals->length < length) {
            fs_error(LOG_ERR, "b[%d].length = %d / %d\n", i, b[i].vals->length, length);
        }
#endif
    }
    if (!scount) {
	fs_error(LOG_WARNING, "fs_binding_sort() called with no sort "
			      "columns set, ignoring");

	return;
    }

    fs_binding_sort_intl(b, 0, length - 1);
}

void fs_binding_uniq(fs_binding *b)
{
    int length = fs_binding_length(b);

    int outrow = 0;
    for (int row = 0; row < length; row++) {
	if (row < length - 1 && binding_row_compare(NULL, b, b, row, row+1, length, length, 0) == 0) {
	    continue;
	}
	for (int column = 0; b[column].name; column++) {
	    if (b[column].sort) {
		b[column].vals->data[outrow] = b[column].vals->data[row];
	    }
	}
	outrow++;
    }
    for (int column = 0; b[column].name; column++) {
	if (b[column].sort) {
	    b[column].vals->length = outrow;
	}
    }
}

/* truncate a binding to length entries long */
void fs_binding_truncate(fs_binding *b, int length)
{
    for (int i=0; b[i].name; i++) {
        fs_rid_vector_truncate(b[i].vals, length);
    }
}

/* perform the cross product on two binding tables */

void fs_binding_merge(fs_query *q, int block, fs_binding *from, fs_binding *to, char *vars[], int num_vars, int flags)
{
    fs_binding *inter_f = NULL; /* the intersecting column */
    fs_binding *inter_t = NULL; /* the intersecting column */

    for (int i=0; from[i].name; i++) {
	from[i].sort = 0;
	to[i].sort = 0;
    }
#if DEBUG_MERGE > 2
    printf("@@ block %d\n", block);
#endif
    int used_first_in_union = 0;
    int used = 0;
    for (int i=0; from[i].name; i++) {
	if (!from[i].bound || !to[i].bound) continue;
	const int first_in_union = (flags & FS_BIND_UNION) &&
	      from[i].bound_in_block[block] == 1;
#if DEBUG_MERGE > 2
        printf("@@ FIU %s: %d && %d != 0 && %d == 1\n", to[i].name, flags & FS_BIND_UNION, to[i].appears, from[i].bound_in_block[block]);
#endif
        if (from[i].used) used++;
        if (first_in_union && from[i].used) {
	    used_first_in_union++;
        }

#if DEBUG_MERGE > 2
        printf("@@ JOIN %s ? BOUND:%d,%d && !FIU:%d\n", from[i].name, from[i].bound, to[i].bound, !first_in_union);
#endif
	if (from[i].bound && to[i].bound && !first_in_union) {
	    inter_f = from+i;
	    inter_t = to+i;
	    from[i].sort = 1;
	    to[i].sort = 1;
#ifdef DEBUG_MERGE
    printf("@@ join on %s\n", to[i].name);
#endif
	}
    }

#if DEBUG_MERGE > 2
    printf("@@ DUMP? %d && %d == %d && %d > 0\n", !inter_f, used, used_first_in_union, used);
#endif
    /* from and to bound variables do not intersect, we can just dump results,
       under some circustances we need to so a combinatorial explosion */
    if (!inter_f && (fs_binding_length(from) == 0 || (used == used_first_in_union && used > 0))) {
	const int length_f = fs_binding_length(from);
	const int length_t = fs_binding_length(to);
	for (int i=0; 1; i++) {
	    if (!from[i].name) break;
	    if (to[i].bound && !from[i].bound) {
                if (from[i].vals && from[i].ubs) {
                    fs_rid_vector_free(from[i].vals);
                    fs_rid_vector_free(from[i].ubs);
                }
		from[i].vals = fs_rid_vector_new(length_f);
		for (int d=0; d<length_f; d++) {
		    from[i].vals->data[d] = FS_RID_NULL;
		}
		from[i].ubs = fs_rid_vector_new(length_f);
		for (int d=0; d<length_f; d++) {
		    from[i].ubs->data[d] = 0;
		}
		from[i].bound = 1;
	    }
	    if (!from[i].bound) continue;
	    if (!to[i].bound) {
                if (to[i].vals && to[i].ubs) {
                    fs_rid_vector_free(to[i].vals);
                    fs_rid_vector_free(to[i].ubs);
                }
		to[i].vals = fs_rid_vector_new(length_t);
		for (int d=0; d<length_t; d++) {
                    to[i].vals->data[d] = FS_RID_NULL;
                }
		to[i].ubs = fs_rid_vector_new(length_t);
		for (int d=0; d<length_t; d++) {
                    to[i].ubs->data[d] = 0;
		}
	    }
	    to[i].bound_in_block[block] += from[i].bound_in_block[block];
	    fs_rid_vector_append_vector(to[i].vals, from[i].vals);
	    fs_rid_vector_append_vector(to[i].ubs, from[i].ubs);
	    to[i].bound = 1;
	}
#ifdef DEBUG_MERGE
    printf("append all, result:\n");
    fs_binding_print(to, stdout);
#endif

	return;
    }

    int length_t = fs_binding_length(to);
    int length_f = fs_binding_length(from);
    for (int i=0; to[i].name; i++) {
	int bound_in_this_union = 0;
	if (q->union_group[block] > 0 &&
	    q->union_group[block] == q->union_group[from[i].appears] &&
	    from[i].bound_in_block[block] == 1) {
	    bound_in_this_union = 1;
	}
	if (to+i == inter_t || to[i].used || to[i].bound) {
	    /* do nothing */
#ifdef DEBUG_MERGE
    printf("@@ preserve %s\n", to[i].name);
#endif
	} else if (from[i].bound && (!bound_in_this_union || !to[i].bound)) {
#ifdef DEBUG_MERGE
    printf("@@ replace %s\n", from[i].name);
#endif
	    to[i].bound = 1;
            if (to[i].vals || to[i].ubs) {
                fs_rid_vector_free(to[i].vals);
                fs_rid_vector_free(to[i].ubs);
            }
	    to[i].vals = fs_rid_vector_new(length_t);
	    for (int d=0; d<length_t; d++) {
		to[i].vals->data[d] = FS_RID_NULL;
	    }
	    to[i].ubs = fs_rid_vector_new(length_t);
	    for (int d=0; d<length_t; d++) {
		to[i].ubs->data[d] = 0;
	    }
	}
    }

    /* sort the two sets of bindings so they can be merged linearly */
    if (inter_t) {
	fs_binding_sort(from);
	fs_binding_sort(to);
    }

#ifdef DEBUG_MERGE
    printf("old: %d bindings\n", fs_binding_length(from));
    fs_binding_print(from, stdout);
    printf("new: %d bindings\n", fs_binding_length(to));
    fs_binding_print(to, stdout);
#endif

    int fpos = 0;
    int tpos = 0;
    /* If were running in restricted mode, truncate the binding tables */
    if (q->flags & FS_QUERY_RESTRICTED) {
        fs_binding_truncate(from, q->soft_limit);
        fs_binding_truncate(to, q->soft_limit);
    }
    while (fpos < length_f || tpos < length_t) {
        if (q->flags & FS_QUERY_RESTRICTED &&
            fs_binding_length(to) >= q->soft_limit) {
            char *msg = g_strdup("some results have been dropped to prevent overunning time allocation");
            q->warnings = g_slist_prepend(q->warnings, msg);
            break;
        }
	int cmp;
	cmp = binding_row_compare(q, from, to, fpos, tpos, length_f, length_t, flags);
	if (cmp == 0) {
	    /* both rows match */
	    int fp, tp = tpos;
	    for (fp = fpos; binding_row_compare(q, from, to, fp, tpos, length_f, length_t, flags) == 0; fp++) {
#if DEBUG_MERGE > 1
if (fp == 20) {
    printf("...\n");
}
#endif
		for (tp = tpos; 1; tp++) {
		    if (binding_row_compare(q, from, to, fp, tp, length_f, length_t, flags) == 0) {
#if DEBUG_MERGE > 1
if (fp < 20) {
    printf("STEP %d, %d  ", fp-fpos, tp-tpos);
}
#endif
			if (fp == fpos) {
#if DEBUG_MERGE > 1
if (fp < 20) {
    if (inter_f) {
	printf("REPL %llx\n", inter_f->vals->data[fp]);
    } else {
	printf("REPL ???\n");
    }
}
#endif
			    for (int c=0; to[c].name; c++) {
				if (!from[c].bound && !to[c].bound) continue;
				if (from[c].bound &&
				    from[c].vals->data[fp] == FS_RID_NULL) {
				    continue;
				}
				if (from[c].bound && fp < from[c].vals->length) {
				    to[c].vals->data[tp] = from[c].vals->data[fp];
				    to[c].ubs->data[tp] = from[c].ubs->data[fp];
				    if (to[c].vals->length <= tp) {
					to[c].vals->length = tp+1;
					to[c].ubs->length = tp+1;
				    }
				}
			    }
			} else {
#if DEBUG_MERGE > 1
if (fp < 20) {
    printf("ADD\n");
}
#endif
			    for (int c=0; to[c].name; c++) {
				if (!from[c].bound && !to[c].bound) continue;
				if (from[c].bound && fp < from[c].vals->length) {
				    fs_rid_vector_append(to[c].vals, from[c].vals->data[fp]);
				    fs_rid_vector_append(to[c].ubs, from[c].ubs->data[fp]);
				} else {
				    fs_rid_vector_append(to[c].vals, to[c].vals->data[tp]);
				    if (q->union_group[block] > 0 &&
					q->union_group[block] == q->union_group[from[c].appears]) {
					fs_rid_vector_append(to[c].ubs, block);
				    } else {
					fs_rid_vector_append(to[c].ubs, 0);
				    }
				}
			    }
			}
		    } else {
			break;
		    }
		}
	    }
	    tpos = tp;
	    fpos = fp;
	} else if (cmp == -1) {
#if DEBUG_MERGE > 1
if (fpos < 20) {
    if (inter_f) {
	printf("DUP F %s=%llx (%d = %d) %s\n", inter_f->name, inter_f->vals->data[fpos], fpos, tpos, flags & FS_BIND_OPTIONAL ? "opt" : "reqd");
    } else {
	printf("DUP F ??? (%d = %d) %s\n", fpos, tpos, flags & FS_BIND_OPTIONAL ? "opt" : "reqd");
    }
}
#endif
	    /* we might need to duplicate the from row */
	    if (flags & (FS_BIND_OPTIONAL /*| FS_BIND_UNION*/)) {
		for (int c=0; to[c].name; c++) {
		    if (!from[c].bound && !to[c].bound) continue;
		    if (from[c].bound && fpos < from[c].vals->length) {
			fs_rid_vector_append(to[c].vals,
                                             from[c].vals->data[fpos]);
			fs_rid_vector_append(to[c].ubs,
                                             from[c].ubs->data[fpos]);
		    } else {
			fs_rid_vector_append(to[c].vals, FS_RID_NULL);
			fs_rid_vector_append(to[c].ubs, 0);
		    }
		}
	    }
	    fpos++;
	} else if (cmp == 1) {
#if DEBUG_MERGE > 1
if (fpos < 20) {
    printf("DUP T %llx (%d = %d) %s\n", inter_f->vals->data[fpos], fpos, tpos, flags & FS_BIND_OPTIONAL ? "opt" : "reqd");
}
#endif
	    /* we need to duplicate the to row */
	    if (flags & (FS_BIND_OPTIONAL /*| FS_BIND_UNION*/)) {
		for (int c=0; to[c].name; c++) {
		    if (!from[c].bound && !to[c].bound) continue;
		    if (from[c].bound && fpos < from[c].vals->length) {
			fs_rid_vector_append(to[c].vals, from[c].vals->data[fpos]);
			if (q->union_group[block] > 0 &&
			    q->union_group[block] == q->union_group[from[c].appears]) {
			    fs_rid_vector_append(to[c].ubs, from[c].ubs->data[fpos]);
			} else {
			    fs_rid_vector_append(to[c].ubs, 0);
			}
		    } else {
			fs_rid_vector_append(to[c].vals, FS_RID_NULL);
			fs_rid_vector_append(to[c].ubs, 0);
		    }
		}
	    }
	    tpos++;
	} else if (cmp == 2) {
#if DEBUG_MERGE > 1
if (fpos < 20) {
    printf("DUP U %llx (%d = %d) %s\n", inter_f->vals->data[fpos], fpos, tpos, flags & FS_BIND_OPTIONAL ? "opt" : "reqd");
}
#endif
	    for (int c=0; to[c].name; c++) {
		if (!from[c].bound && !to[c].bound) continue;
		if (from[c].bound && fpos < from[c].vals->length) {
		    fs_rid_vector_append(to[c].vals, from[c].vals->data[fpos]);
		    fs_rid_vector_append(to[c].ubs, from[c].ubs->data[fpos]);
		} else {
		    fs_rid_vector_append(to[c].vals, FS_RID_NULL);
		    fs_rid_vector_append(to[c].ubs, 0);
		}
	    }
	    fpos++;
	    tpos++;
	} else {
	    fs_error(LOG_CRIT, "unknown compare state in biding");
	}
    }

#ifdef DEBUG_MERGE
    printf("result: %d bindings\n", fs_binding_length(to));
    fs_binding_print(to, stdout);
#endif
}

/* vi:set expandtab sts=4 sw=4: */