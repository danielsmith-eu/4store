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


    Copyright 2011 Manuel Salvadores
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include <rasqal.h>

#include "../common/4store.h"
#include "../common/error.h"


static int is_open_triple(rasqal_triple *t,rasqal_literal *model) {
    return t->subject->type == RASQAL_LITERAL_VARIABLE &&
    t->predicate->type == RASQAL_LITERAL_VARIABLE &&
    t->object->type == RASQAL_LITERAL_VARIABLE 
    &&  (!model || model->type != RASQAL_LITERAL_URI);
}

static int has_regex(rasqal_expression *e)
{
    switch (e->op) {
        case RASQAL_EXPR_AND:
        case RASQAL_EXPR_OR:
            return has_regex(e->arg1) || 
                         has_regex(e->arg2);
        case RASQAL_EXPR_REGEX:
            return 1;
        default:
            break;
    }
    return 0;
}

static int graph_pattern_check(rasqal_graph_pattern *pattern, rasqal_literal *model)
{
    if (!pattern) {
	return 0;
    }


    int op = rasqal_graph_pattern_get_operator(pattern);
    int handled = 0;
    int regex = 0;
    int open = 0;

    switch (op) {
    case RASQAL_GRAPH_PATTERN_OPERATOR_OPTIONAL:
        break;
    case RASQAL_GRAPH_PATTERN_OPERATOR_UNION:
        break;
    case RASQAL_GRAPH_PATTERN_OPERATOR_BASIC:
    case RASQAL_GRAPH_PATTERN_OPERATOR_GRAPH:
    case RASQAL_GRAPH_PATTERN_OPERATOR_GROUP:
        handled = 1;
        if (op == RASQAL_GRAPH_PATTERN_OPERATOR_GRAPH) {
            model = rasqal_graph_pattern_get_origin(pattern);
            if (!model) {
                printf("expected origin from pattern, but got NULL\n");
            }
        }
        break;
    case RASQAL_GRAPH_PATTERN_OPERATOR_FILTER:
        handled = 1;
        rasqal_expression *e =
            rasqal_graph_pattern_get_filter_expression(pattern);
            regex = has_regex(e);
        break;
    default:
        break;
    }
    if (!handled) {
	printf("Unknown GP operator %d not supported\n", op);
    }


    for (int i=0; 1; i++) {
        rasqal_triple *rt = rasqal_graph_pattern_get_triple(pattern, i);
        if (!rt) break;
        open = open || is_open_triple(rt,model);
        if (open)
            continue;
    }
    if (open || regex)
        return 1;
    
    int open_regex=0;
    for (int index=0; 1; index++) {
        rasqal_graph_pattern *sgp =
                    rasqal_graph_pattern_get_sub_graph_pattern(pattern, index);
        if (!sgp) break;
        open_regex = graph_pattern_check(sgp, model);
        if (open_regex)
            return open_regex;
    }
    return open_regex;
}

/* to detect SELECT DISCTINCT ?g WHERE { GRAPH ?g { ?s ?p ?o } } */
static int is_distinct_graph_or_predicate(rasqal_query *rq) {
    if (!rasqal_query_get_distinct(rq)) 
        return 0;
    
    if (rasqal_query_get_verb(rq) != RASQAL_QUERY_VERB_SELECT)
        return 0;
    
    raptor_sequence *vars = rasqal_query_get_bound_variable_sequence(rq);
    int nvars = raptor_sequence_size(vars);
    if (nvars != 1)
        return 0; 

    rasqal_graph_pattern *pattern = rasqal_query_get_query_graph_pattern(rq);
    
    rasqal_graph_pattern *sgp =
                rasqal_graph_pattern_get_sub_graph_pattern(pattern, 0);
    for (int i=0; 1; i++) {
        rasqal_triple *rt = rasqal_graph_pattern_get_triple(pattern, i);
        if (rt && sgp) 
            return 0;
        break;
    }

    if (rasqal_graph_pattern_get_filter_expression(pattern))
        return 0;
    

    if (sgp && rasqal_graph_pattern_get_filter_expression(sgp))
        return 0;

    if (rasqal_graph_pattern_get_sub_graph_pattern(pattern, 1) ||
        (sgp && rasqal_graph_pattern_get_sub_graph_pattern(sgp, 0)))
        return 0;

    int nt=0;
    rasqal_triple *rtg = NULL;
    if (!sgp) {
        sgp = pattern;
    }
    for (int i=0; 1; i++) {
        rasqal_triple *rt = rasqal_graph_pattern_get_triple(sgp, i);
        if (!rt) break;
        nt++;
        if (nt>1)
            return 0;
        if (!(rt->subject->type == RASQAL_LITERAL_VARIABLE &&
                rt->predicate->type == RASQAL_LITERAL_VARIABLE &&
                rt->object->type == RASQAL_LITERAL_VARIABLE))
                    return 0;
         rtg=rt;
    }
        printf("SGP %p\n",sgp);
    rasqal_literal *model = rasqal_graph_pattern_get_origin(pattern);
    if (model && (raptor_sequence_get_at(vars, 0) == model->value.variable))
        return 1;
    if (raptor_sequence_get_at(vars, 0) == rtg->predicate->value.variable)
        return 2;
    return 0;
}


int fs_check_query_forbiden(rasqal_query *rq) {
    rasqal_graph_pattern *pattern = rasqal_query_get_query_graph_pattern(rq);
    if (graph_pattern_check(pattern,NULL)) {
        if (is_distinct_graph_or_predicate(rq))
            return 0;
        return 1;
    }
    return 0;
}
