#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "tree-pass.h"
#include "context.h"
#include "cgraph.h"
#include "diagnostic.h"
#include "tree-cfg.h"
#include "tree-ssa.h"
#include "tree-ssa-operands.h"
#include "stringpool.h"
#include "attribs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int plugin_is_GPL_compatible;

using err_t = long long;

enum error_type : err_t {
    ErrorOk = 0,
    ErrorWeakAssert = 1,
    ErrorStrongAssert = 2,
    ErrorGccLogic = 4,
    ErrorCustomLogic = 8,
    ErrorMemResource = 16,
    ErrorIo = 32,
    ErrorGc = 64
};

#define PATTERN_BEGIN err_t ret = ErrorOk;
#define PATTERN_SAFE_CHECK_LABEL(allow_error, label) do { if (ret != ErrorOk && ret != (allow_error)) { goto label; } } while (0)
#define PATTERN_SAFE_CHECK_STRONG_LABEL(label) PATTERN_SAFE_CHECK_LABEL(ErrorOk, label)
#define PATTERN_SAFE_CHECK_WEAK_LABEL(label) PATTERN_SAFE_CHECK_LABEL(ErrorWeakAssert, label)
#define PATTERN_SAFE_CHECK_STRONG() PATTERN_SAFE_CHECK_STRONG_LABEL(fn_final)
#define PATTERN_SAFE_CHECK_WEAK() PATTERN_SAFE_CHECK_WEAK_LABEL(fn_final)
#define PATTERN_MATCH_RAW(x, y, e) do { if ((x) == 0) { (x) = (y); } else if ((x) != (y)) { ret = (e); } } while (0)
#define PATTERN_MATCH_STRONG(x, y) PATTERN_MATCH_RAW(x, y, ErrorStrongAssert)
#define PATTERN_MATCH_WEAK(x, y) PATTERN_MATCH_RAW(x, y, ErrorWeakAssert)
#define PATTERN_WRAP(x) do { ret |= (x); } while (0)
#define PATTERN_END do { fn_final: return ret; } while (0)
#define DEBUG(fmt, ...) fprintf(stderr, "%s:%d %s: " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define PATTERN_STRIP_WEAK() do { ret &= ~ErrorWeakAssert; } while (0)

struct vector_candidate {
    tree type;
    tree ptr_field;
    tree size_field;
    tree capacity_field;
    bool has_ptr_access;
    bool has_size_check;
    bool has_capacity_check;
    bool ptr_escapes;
};

struct access_record {
    gimple *stmt;
    basic_block bb;
    char const *function_name;
    location_t location;
    struct access_record *next;
};

struct Analysis {
    struct vector_candidate *candidates;
    int candidate_count;
    int candidate_capacity;
    struct access_record *access_head;
    FILE *output_file;
  
    err_t init();
    err_t analyze_function(cgraph_node *node);
    err_t analyze_gimple_stmt(gimple *stmt, char const *fn_name);
    err_t check_mem_ref(tree expr, char const *fn_name, gimple *stmt);
    err_t check_comparisons(gimple *stmt);
    err_t identify_vector_types();
    err_t perform_escape_analysis();
    err_t output_results();
    err_t add_candidate(tree type);
    err_t add_access_record(gimple *stmt, basic_block bb, char const *fn_name);
};

err_t Analysis::init() {
    PATTERN_BEGIN;
  
    candidates = NULL;
    candidate_count = 0;
    candidate_capacity = 0;
    access_head = NULL;
    output_file = NULL;
  
    candidate_capacity = 128;
    candidates = (struct vector_candidate*)xcalloc(candidate_capacity, sizeof(struct vector_candidate));
    if (!candidates) {
        ret = ErrorMemResource;
        PATTERN_SAFE_CHECK_STRONG();
    }
  
    output_file = fopen("vector_pattern_analysis.json", "w");
    if (!output_file) {
        ret = ErrorIo;
        PATTERN_SAFE_CHECK_STRONG();
    }
  
    PATTERN_END;
}

err_t Analysis::add_candidate(tree type) {
    PATTERN_BEGIN;
  
    int i;
    tree field;
    struct vector_candidate *cand;
  
    if (!type || TREE_CODE(type) != RECORD_TYPE) {
        ret = ErrorWeakAssert;
        PATTERN_SAFE_CHECK_WEAK();
    }
  
    for (i = 0; i < candidate_count; i++) {
        if (candidates[i].type == type) {
            goto fn_final;
        }
    }
  
    if (candidate_count >= candidate_capacity) {
        candidate_capacity *= 2;
        candidates = (struct vector_candidate*)xrealloc(candidates, 
            candidate_capacity * sizeof(struct vector_candidate));
        if (!candidates) {
            ret = ErrorMemResource;
            PATTERN_SAFE_CHECK_STRONG();
        }
    }
  
    cand = &candidates[candidate_count];
    memset(cand, 0, sizeof(struct vector_candidate));
    cand->type = type;
  
    for (field = TYPE_FIELDS(type); field; field = DECL_CHAIN(field)) {
        if (TREE_CODE(field) != FIELD_DECL) continue;
      
        tree field_type = TREE_TYPE(field);
        if (POINTER_TYPE_P(field_type) && !cand->ptr_field) {
            cand->ptr_field = field;
        } else if (INTEGRAL_TYPE_P(field_type)) {
            if (!cand->size_field) {
                cand->size_field = field;
            } else if (!cand->capacity_field) {
                cand->capacity_field = field;
            }
        }
    }
  
    if (cand->ptr_field && cand->size_field && cand->capacity_field) {
        candidate_count++;
    }
  
    PATTERN_END;
}

err_t Analysis::add_access_record(gimple *stmt, basic_block bb, char const *fn_name) {
    PATTERN_BEGIN;
  
    struct access_record *record = NULL;
  
    record = (struct access_record*)xcalloc(1, sizeof(struct access_record));
    if (!record) {
        ret = ErrorMemResource;
        PATTERN_SAFE_CHECK_STRONG();
    }
  
    record->stmt = stmt;
    record->bb = bb;
    record->function_name = fn_name;
    record->location = gimple_location(stmt);
    record->next = access_head;
    access_head = record;
  
    PATTERN_END;
}

err_t Analysis::check_mem_ref(tree expr, char const *fn_name, gimple *stmt) {
    PATTERN_BEGIN;
  
    tree base;
    tree offset;
    tree ptr_type;
    tree record_type;
  
    if (!expr || TREE_CODE(expr) != MEM_REF) {
        goto fn_final;
    }
  
    base = TREE_OPERAND(expr, 0);
    offset = TREE_OPERAND(expr, 1);
  
    while (base) {
        if (TREE_CODE(base) == SSA_NAME) {
            gimple *def_stmt = SSA_NAME_DEF_STMT(base);
            if (def_stmt && is_gimple_assign(def_stmt)) {
                tree rhs = gimple_assign_rhs1(def_stmt);
                if (TREE_CODE(rhs) == COMPONENT_REF) {
                    tree field = TREE_OPERAND(rhs, 1);
                    tree obj = TREE_OPERAND(rhs, 0);
                    ptr_type = TREE_TYPE(obj);
                  
                    if (ptr_type && TREE_CODE(ptr_type) == POINTER_TYPE) {
                        record_type = TREE_TYPE(ptr_type);
                    } else {
                        record_type = ptr_type;
                    }
                  
                    if (record_type && TREE_CODE(record_type) == RECORD_TYPE) {
                        PATTERN_WRAP(add_candidate(record_type));
                        PATTERN_SAFE_CHECK_WEAK();
                      
                        int i;
                        for (i = 0; i < candidate_count; i++) {
                            if (candidates[i].type == record_type && 
                                candidates[i].ptr_field == field) {
                                candidates[i].has_ptr_access = true;
                                PATTERN_WRAP(add_access_record(stmt, 
                                    gimple_bb(stmt), fn_name));
                                break;
                            }
                        }
                    }
                    break;
                }
                base = rhs;
            } else {
                break;
            }
        } else if (TREE_CODE(base) == COMPONENT_REF) {
            tree field = TREE_OPERAND(base, 1);
            tree obj = TREE_OPERAND(base, 0);
            ptr_type = TREE_TYPE(obj);
          
            if (ptr_type && TREE_CODE(ptr_type) == POINTER_TYPE) {
                record_type = TREE_TYPE(ptr_type);
            } else {
                record_type = ptr_type;
            }
          
            if (record_type && TREE_CODE(record_type) == RECORD_TYPE) {
                PATTERN_WRAP(add_candidate(record_type));
                PATTERN_SAFE_CHECK_WEAK();
              
                int i;
                for (i = 0; i < candidate_count; i++) {
                    if (candidates[i].type == record_type && 
                        candidates[i].ptr_field == field) {
                        candidates[i].has_ptr_access = true;
                        PATTERN_WRAP(add_access_record(stmt, 
                            gimple_bb(stmt), fn_name));
                        break;
                    }
                }
            }
            break;
        } else {
            break;
        }
    }
  
    PATTERN_END;
}

err_t Analysis::check_comparisons(gimple *stmt) {
    PATTERN_BEGIN;
  
    tree lhs;
    tree rhs;
    enum tree_code code;
  
    if (!is_gimple_assign(stmt)) {
        goto fn_final;
    }
  
    code = gimple_assign_rhs_code(stmt);
    if (code != LT_EXPR && code != LE_EXPR && code != GT_EXPR && code != GE_EXPR) {
        goto fn_final;
    }
  
    lhs = gimple_assign_rhs1(stmt);
    rhs = gimple_assign_rhs2(stmt);
  
    int i;
    for (i = 0; i < candidate_count; i++) {
        struct vector_candidate *cand = &candidates[i];
      
        if ((lhs && TREE_CODE(lhs) == COMPONENT_REF && 
             TREE_OPERAND(lhs, 1) == cand->size_field) ||
            (rhs && TREE_CODE(rhs) == COMPONENT_REF && 
             TREE_OPERAND(rhs, 1) == cand->size_field)) {
            cand->has_size_check = true;
        }
      
        if ((lhs && TREE_CODE(lhs) == COMPONENT_REF && 
             TREE_OPERAND(lhs, 1) == cand->capacity_field) ||
            (rhs && TREE_CODE(rhs) == COMPONENT_REF && 
             TREE_OPERAND(rhs, 1) == cand->capacity_field)) {
            cand->has_capacity_check = true;
        }
    }
  
    PATTERN_END;
}

err_t Analysis::analyze_gimple_stmt(gimple *stmt, char const *fn_name) {
    PATTERN_BEGIN;
  
    tree lhs;
    tree rhs;
  
    if (!stmt) {
        goto fn_final;
    }
  
    if (is_gimple_assign(stmt)) {
        lhs = gimple_assign_lhs(stmt);
        PATTERN_WRAP(check_mem_ref(lhs, fn_name, stmt));
        PATTERN_SAFE_CHECK_WEAK();
      
        rhs = gimple_assign_rhs1(stmt);
        PATTERN_WRAP(check_mem_ref(rhs, fn_name, stmt));
        PATTERN_SAFE_CHECK_WEAK();
      
        PATTERN_WRAP(check_comparisons(stmt));
        PATTERN_SAFE_CHECK_WEAK();
    }
  
    PATTERN_END;
}

err_t Analysis::analyze_function(cgraph_node *node) {
    PATTERN_BEGIN;
  
    function *fn;
    basic_block bb;
    gimple_stmt_iterator gsi;
    char const *fn_name;
  
    if (!node || !node->has_gimple_body_p()) {
        goto fn_final;
    }
  
    fn = DECL_STRUCT_FUNCTION(node->decl);
    if (!fn) {
        goto fn_final;
    }
  
    fn_name = IDENTIFIER_POINTER(DECL_NAME(node->decl));
    if (!fn_name) {
        fn_name = "<anonymous>";
    }
  
    push_cfun(fn);
  
    FOR_EACH_BB_FN(bb, fn) {
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            PATTERN_WRAP(analyze_gimple_stmt(stmt, fn_name));
            PATTERN_SAFE_CHECK_WEAK();
        }
    }
  
    pop_cfun();
  
    PATTERN_END;
}

err_t Analysis::perform_escape_analysis() {
    PATTERN_BEGIN;
  
    int i;
    cgraph_node *node;
  
    for (i = 0; i < candidate_count; i++) {
        candidates[i].ptr_escapes = false;
    }
  
    FOR_EACH_FUNCTION(node) {
        function *fn;
        basic_block bb;
        gimple_stmt_iterator gsi;
      
        if (!node->has_gimple_body_p()) continue;
      
        fn = DECL_STRUCT_FUNCTION(node->decl);
        if (!fn) continue;
      
        push_cfun(fn);
      
        FOR_EACH_BB_FN(bb, fn) {
            for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
                gimple *stmt = gsi_stmt(gsi);
              
                if (is_gimple_call(stmt)) {
                    unsigned j;
                    for (j = 0; j < gimple_call_num_args(stmt); j++) {
                        tree arg = gimple_call_arg(stmt, j);
                        if (arg && TREE_CODE(arg) == COMPONENT_REF) {
                            tree field = TREE_OPERAND(arg, 1);
                            int k;
                            for (k = 0; k < candidate_count; k++) {
                                if (candidates[k].ptr_field == field) {
                                    candidates[k].ptr_escapes = true;
                                }
                            }
                        }
                    }
                }
            }
        }
      
        pop_cfun();
    }
  
    PATTERN_END;
}

err_t Analysis::output_results() {
    PATTERN_BEGIN;
  
    int i;
    struct access_record *rec;
  
    if (!output_file) {
        ret = ErrorIo;
        PATTERN_SAFE_CHECK_STRONG();
    }
  
    fprintf(output_file, "{\n");
    fprintf(output_file, "  \"vector_patterns\": [\n");
  
    for (i = 0; i < candidate_count; i++) {
        struct vector_candidate *cand = &candidates[i];
      
        if (!cand->has_ptr_access || cand->ptr_escapes) continue;
      
        fprintf(output_file, "    {\n");
      
        tree type_name = TYPE_NAME(cand->type);
        if (type_name && TREE_CODE(type_name) == TYPE_DECL) {
            char const *name = IDENTIFIER_POINTER(DECL_NAME(type_name));
            fprintf(output_file, "      \"type_name\": \"%s\",\n", name);
        } else {
            fprintf(output_file, "      \"type_name\": \"<anonymous>\",\n");
        }
      
        if (cand->ptr_field && DECL_NAME(cand->ptr_field)) {
            fprintf(output_file, "      \"ptr_field\": \"%s\",\n", 
                IDENTIFIER_POINTER(DECL_NAME(cand->ptr_field)));
        }
      
        if (cand->size_field && DECL_NAME(cand->size_field)) {
            fprintf(output_file, "      \"size_field\": \"%s\",\n", 
                IDENTIFIER_POINTER(DECL_NAME(cand->size_field)));
        }
      
        if (cand->capacity_field && DECL_NAME(cand->capacity_field)) {
            fprintf(output_file, "      \"capacity_field\": \"%s\",\n", 
                IDENTIFIER_POINTER(DECL_NAME(cand->capacity_field)));
        }
      
        fprintf(output_file, "      \"has_size_check\": %s,\n", 
            cand->has_size_check ? "true" : "false");
        fprintf(output_file, "      \"has_capacity_check\": %s,\n", 
            cand->has_capacity_check ? "true" : "false");
        fprintf(output_file, "      \"ptr_escapes\": %s\n", 
            cand->ptr_escapes ? "true" : "false");
      
        fprintf(output_file, "    }%s\n", (i < candidate_count - 1) ? "," : "");
    }
  
    fprintf(output_file, "  ],\n");
    fprintf(output_file, "  \"access_records\": [\n");
  
    for (rec = access_head; rec; rec = rec->next) {
        fprintf(output_file, "    {\n");
        fprintf(output_file, "      \"function\": \"%s\",\n", rec->function_name);
        fprintf(output_file, "      \"bb_index\": %d", rec->bb->index);
      
        if (rec->location != UNKNOWN_LOCATION) {
            expanded_location xloc = expand_location(rec->location);
            fprintf(output_file, ",\n      \"file\": \"%s\",\n", xloc.file);
            fprintf(output_file, "      \"line\": %d", xloc.line);
        }
      
        fprintf(output_file, "\n    }%s\n", rec->next ? "," : "");
    }
  
    fprintf(output_file, "  ]\n");
    fprintf(output_file, "}\n");
  
    fclose(output_file);
    output_file = NULL;
  
    PATTERN_END;
}

static struct Analysis analysis;

static unsigned int vector_pattern_execute() {
    PATTERN_BEGIN;
  
    cgraph_node *node;
  
    PATTERN_WRAP(analysis.init());
    PATTERN_SAFE_CHECK_STRONG();
  
    FOR_EACH_FUNCTION(node) {
        if (!node->inlined_to) {
            PATTERN_WRAP(analysis.analyze_function(node));
            PATTERN_SAFE_CHECK_WEAK();
        }
    }
  
    PATTERN_WRAP(analysis.perform_escape_analysis());
    PATTERN_SAFE_CHECK_WEAK();
  
    PATTERN_WRAP(analysis.output_results());
    PATTERN_SAFE_CHECK_STRONG();
  
    PATTERN_STRIP_WEAK();
  
    fn_final:
    return (ret == ErrorOk) ? 0 : 1;
}

namespace {

const pass_data vector_pattern_pass_data = {
    IPA_PASS,
    "vector_pattern",
    OPTGROUP_NONE,
    TV_NONE,
    PROP_cfg,
    0,
    0,
    0,
    0
};

class vector_pattern_pass : public ipa_opt_pass_d {
public:
    vector_pattern_pass(gcc::context *ctxt)
        : ipa_opt_pass_d(vector_pattern_pass_data, ctxt,
                         NULL, NULL, NULL, NULL, NULL, NULL,
                         0, NULL, NULL) {
    }
  
    virtual unsigned int execute(function *) override {
        return vector_pattern_execute();
    }
};

}

static void register_vector_pattern_pass(void *gcc_data, void *user_data) {
    struct register_pass_info pass_info;
  
    pass_info.pass = new vector_pattern_pass(g);
    pass_info.reference_pass_name = "whole-program";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
  
    register_callback("vector_pattern", PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
}

int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        error("This plugin requires GCC 15");
        return 1;
    }
  
    register_callback(plugin_info->base_name, PLUGIN_ALL_IPA_PASSES_START,
                     register_vector_pattern_pass, NULL);
  
    return 0;
}
