#include "gcc-plugin.h"
#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "tree-pass.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "context.h"
#include "print-tree.h"
#include "cgraph.h"
#include "vec.h"
#include "hash-table.h"

int plugin_is_GPL_compatible;

using err_t = long long;

enum error_type : err_t {
    ErrorOk,
    ErrorWeakAssert,
    ErrorStrongAssert,
    ErrorGccLogic,
    ErrorCustomLogic,
    ErrorMemResource,
    ErrorIo
};

#define PATTERN_BEGIN err_t ret = ErrorOk;
#define PATTERN_SAFE_CHECK_STRONG() do { if (ret != ErrorOk) { return ret; } } while (0)
#define PATTERN_SAFE_CHECK_WEAK() do { if (ret == ErrorStrongAssert || ret == ErrorGccLogic) { return ret; } } while (0)
#define PATTERN_MATCH(x, y, e) do { if ((x) == 0) { (x) = (y); } else if ((x) != (y)) { ret = (e); } } while (0)
#define PATTERN_MATCH_STRONG(x, y) PATTERN_MATCH(x, y, ErrorStrongAssert)
#define PATTERN_MATCH_WEAK(x, y) PATTERN_MATCH(x, y, ErrorWeakAssert)
#define PATTERN_WRAP(x) do { err_t _tmp = (x); if (_tmp != ErrorOk) { ret = _tmp; } } while (0)
#define PATTERN_END do { return ret; } while (0)
#define DEBUG(fmt, ...) do { fprintf(stderr, "%s:%d %s: " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__); } while (0)

struct PointerAccessInfo {
    tree base_var;
    tree field;
    tree offset;
    tree bound_field;
    gimple* stmt;
    int category;
};

struct Analysis {
    vec<PointerAccessInfo*> accesses;
    hash_table<tree, PointerAccessInfo*> access_map;
    
    err_t init();
    err_t analyze_function(function* fn);
    err_t classify_access(PointerAccessInfo* info);
    err_t collect_pointer_accesses(function* fn);
};

err_t Analysis::init() {
    PATTERN_BEGIN;
    accesses.create(0);
    access_map.create(10);
    PATTERN_END;
}

err_t Analysis::classify_access(PointerAccessInfo* info) {
    PATTERN_BEGIN;
    
    if (info->base_var == NULL_TREE || TREE_CODE(info->base_var) != VAR_DECL) {
        info->category = 2;
        PATTERN_END;
    }
    
    tree type = TREE_TYPE(info->base_var);
    PATTERN_MATCH_STRONG(type, NULL_TREE);
    
    if (info->field != NULL_TREE && info->offset != NULL_TREE) {
        bool offset_proven = false;
        
        if (TREE_CODE(info->offset) == INTEGER_CST) {
            tree bound_val = NULL_TREE;
            if (info->bound_field != NULL_TREE) {
                bound_val = DECL_FIELD_OFFSET(info->bound_field);
                if (bound_val != NULL_TREE && TREE_CODE(bound_val) == INTEGER_CST) {
                    if (tree_int_cst_lt(info->offset, bound_val)) {
                        offset_proven = true;
                    }
                }
            }
            
            if (offset_proven) {
                info->category = 0;
            } else {
                info->category = 1;
            }
        } else {
            info->category = 1;
        }
    } else {
        info->category = 2;
    }
    
    PATTERN_END;
}

static err_t walk_gimple_stmt(gimple* stmt, Analysis* analysis) {
    PATTERN_BEGIN;
    
    if (gimple_assign_single_p(stmt)) {
        tree lhs = gimple_assign_lhs(stmt);
        tree rhs = gimple_assign_rhs1(stmt);
        
        if (TREE_CODE(rhs) == MEM_REF || TREE_CODE(rhs) == INDIRECT_REF) {
            PointerAccessInfo* info = XNEW(PointerAccessInfo);
            memset(info, 0, sizeof(PointerAccessInfo));
            
            info->stmt = stmt;
            info->base_var = TREE_OPERAND(rhs, 0);
            
            if (TREE_CODE(rhs) == MEM_REF) {
                info->offset = TREE_OPERAND(rhs, 1);
            }
            
            PATTERN_WRAP(analysis->classify_access(info));
            analysis->accesses.safe_push(info);
        }
    }
    
    PATTERN_END;
}

err_t Analysis::collect_pointer_accesses(function* fn) {
    PATTERN_BEGIN;
    
    basic_block bb;
    FOR_EACH_BB_FN(bb, fn) {
        gimple_stmt_iterator gsi;
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple* stmt = gsi_stmt(gsi);
            PATTERN_WRAP(walk_gimple_stmt(stmt, this));
        }
    }
    
    PATTERN_END;
}

err_t Analysis::analyze_function(function* fn) {
    PATTERN_BEGIN;
    PATTERN_WRAP(collect_pointer_accesses(fn));
    PATTERN_END;
}

struct ipa_pointer_analysis : public ipa_opt_pass_d {
    Analysis analysis;
    
    ipa_pointer_analysis(gcc::context* ctxt)
        : ipa_opt_pass_d(ipa_pass_pointer_analysis, ctxt) {
        ref_pass_instance_number = 1;
    }
    
    virtual bool gate(function*) { return true; }
    virtual unsigned int execute(function* fn) {
        PATTERN_BEGIN;
        PATTERN_WRAP(analysis.init());
        PATTERN_WRAP(analysis.analyze_function(fn));
        PATTERN_END;
        return 0;
    }
};

extern "C" {
    int plugin_init(struct plugin_name_args* plugin_info,
                   struct plugin_gcc_version* version) {
        if (!plugin_default_version_check(version, &gcc_version))
            return 1;
        
        struct register_pass_info pass_info;
        pass_info.pass = new ipa_pointer_analysis(g);
        pass_info.reference_pass_name = "whole-program";
        pass_info.ref_pass_instance_number = 1;
        pass_info.pos_op = PASS_POS_INSERT_AFTER;
        
        register_callback(plugin_info->base_name,
                         PLUGIN_PASS_MANAGER_SETUP,
                         NULL,
                         &pass_info);
        
        return 0;
    }
}
