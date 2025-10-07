// dynamic_array_scalarization.cpp
#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "tree-pass.h"
#include "context.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "cgraph.h"
#include "diagnostic.h"
#include "vec.h"
#include "hash-map.h"
#include "hash-set.h"

// Plugin information
int plugin_is_GPL_compatible;

// Error codes following DSL rules
enum ErrorCode {
    Uninit = 0,
    Ok,
    RecoverableError,
    RecoverableError1,
    RecoverableError2,
    RecoverableError3,
    SelectOutOfRangeError,
    GccError,
    CustomLogicError,
    MemoryResourceError,
    SystemResourceError,
    SystemError,
    InputOutputError,
    GarbageCollectionError
};

// Global context for managing plugin state
struct CutieContext {
    int64_t error_code;
    int64_t sub_error_code;
    char const* global_error_msg;
    char* custom_temp_error_msg;
    
    // Business data
    hash_set<tree> template_types;
    hash_map<tree, tree> optimized_functions;
    vec<tree> candidate_vectors;
    bool optimization_enabled;
};

// Debug macro following DSL rules
#define CUTIE_DEBUG_INFO(detail) do { \
    fprintf(stderr, "(%s:%d %s [%s])\n", \
            __FILE__, __LINE__, __FUNCTION__, detail); \
} while (0)

// Error handling macros
#define CUTIE_SET_ERROR(ctx, code, sub_code, msg) do { \
    (ctx)->error_code = (code); \
    (ctx)->sub_error_code = (sub_code); \
    (ctx)->global_error_msg = (msg); \
} while (0)

#define CUTIE_CHECK_ERROR(ctx) do { \
    if ((ctx)->error_code != Ok && (ctx)->error_code != Uninit) { \
        CUTIE_DEBUG_INFO("error detected"); \
        return; \
    } \
} while (0)

#define CUTIE_RESET_ERROR(ctx) do { \
    (ctx)->error_code = Ok; \
    (ctx)->sub_error_code = 0; \
} while (0)

// Jump macros for error handling
#define CUTIE_JUMP_ON_ERROR(ctx, label) do { \
    if ((ctx)->error_code != Ok && (ctx)->error_code != Uninit) { \
        goto label; \
    } \
} while (0)

#define CUTIE_TRY_BEGIN(ctx) do { \
    int64_t _saved_error = (ctx)->error_code; \
    int64_t _saved_sub_error = (ctx)->sub_error_code

#define CUTIE_TRY_END(ctx) \
    (ctx)->error_code = _saved_error; \
    (ctx)->sub_error_code = _saved_sub_error; \
} while (0)

// Memory allocation wrapper
#define CUTIE_ALLOC(ctx, type, count) ({ \
    type* _ptr = NULL; \
    _ptr = (type*)ggc_alloc(sizeof(type) * (count)); \
    if (!_ptr) { \
        CUTIE_SET_ERROR(ctx, MemoryResourceError, 0, "allocation failed"); \
    } \
    _ptr; \
})

// Initialize context
void init(CutieContext* ctx) {
    ctx->error_code = Uninit;
    ctx->sub_error_code = 0;
    ctx->global_error_msg = NULL;
    ctx->custom_temp_error_msg = NULL;
    ctx->optimization_enabled = true;
    CUTIE_RESET_ERROR(ctx);
}

// Check if type is template instantiation
void check_template_type(CutieContext* ctx, tree* type, bool* is_template) {
    if (!type || !is_template) {
        CUTIE_SET_ERROR(ctx, CustomLogicError, 0, "null parameter");
        return;
    }
    
    *is_template = false;
    
    if (*type == NULL) {
        // Pattern match: any type
        *is_template = true;
        return;
    }
    
    if (TREE_CODE(*type) != RECORD_TYPE) {
        return;
    }
    
    // Check for template instantiation markers
    tree type_decl = TYPE_NAME(*type);
    if (!type_decl || TREE_CODE(type_decl) != TYPE_DECL) {
        return;
    }
    
    // Look for std::vector pattern
    tree type_id = DECL_NAME(type_decl);
    if (type_id) {
        const char* name = IDENTIFIER_POINTER(type_id);
        if (name && strstr(name, "vector") && strstr(name, "std::")) {
            *is_template = true;
            ctx->template_types.add(*type);
        }
    }
}

// Analyze vector size access
void analyze_vector_size_access(CutieContext* ctx, gimple* stmt, tree* vector_var, bool* is_size_one) {
    if (!stmt || !vector_var || !is_size_one) {
        CUTIE_SET_ERROR(ctx, CustomLogicError, 0, "null parameter");
        return;
    }
    
    *is_size_one = false;
    
    if (*vector_var == NULL) {
        // Pattern match: any vector
        return;
    }
    
    // Check if this is a size() call result
    if (gimple_code(stmt) == GIMPLE_ASSIGN) {
        tree rhs = gimple_assign_rhs1(stmt);
        
        // Look for constant 1
        if (TREE_CODE(rhs) == INTEGER_CST) {
            if (tree_to_uhwi(rhs) == 1) {
                *is_size_one = true;
            }
        }
    }
}

// Create optimized branch
void create_scalar_branch(CutieContext* ctx, gimple_stmt_iterator* gsi, tree* vector_a, tree* vector_b, tree* result) {
    if (!gsi || !vector_a || !vector_b || !result) {
        CUTIE_SET_ERROR(ctx, CustomLogicError, 0, "null parameter");
        return;
    }
    
    CUTIE_DEBUG_INFO("creating scalar optimization branch");
    
    // Create latch variable if not exists
    tree latch_var = create_tmp_var(boolean_type_node, "__builtin_cpp_opt_latch");
    
    // Create condition: latch && a.size() == 1 && b.size() == 1
    tree cond = build3(COND_EXPR, void_type_node, latch_var, NULL_TREE, NULL_TREE);
    
    gimple* cond_stmt = gimple_build_cond(NE_EXPR, latch_var, 
                                          build_int_cst(boolean_type_node, 0),
                                          NULL_TREE, NULL_TREE);
    
    gsi_insert_before(gsi, cond_stmt, GSI_SAME_STMT);
    
    // TODO: Create optimized scalar path
    // This would involve creating direct element access without bounds checking
}

// Walk function to find optimization opportunities
void walk_function(CutieContext* ctx, function* fn) {
    if (!fn) {
        CUTIE_SET_ERROR(ctx, CustomLogicError, 0, "null function");
        return;
    }
    
    basic_block bb;
    FOR_EACH_BB_FN(bb, fn) {
        gimple_stmt_iterator gsi;
        
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple* stmt = gsi_stmt(gsi);
            
            if (!stmt) continue;
            
            // Look for vector operations
            if (is_gimple_call(stmt)) {
                tree fndecl = gimple_call_fndecl(stmt);
                if (fndecl) {
                    const char* name = IDENTIFIER_POINTER(DECL_NAME(fndecl));
                    
                    // Check for vector member functions
                    if (name && (strstr(name, "resize") || 
                               strstr(name, "operator[]") ||
                               strstr(name, "size"))) {
                        ctx->candidate_vectors.safe_push(gimple_call_arg(stmt, 0));
                    }
                }
            }
        }
    }
}

void execute_dynamic_array_scalarization_impl(CutieContext* ctx) {
    CUTIE_DEBUG_INFO("starting dynamic array scalarization");
    
    if (!ctx->optimization_enabled) {
        return ;
    }
    
    // Iterate over all functions
    cgraph_node* node;
    FOR_EACH_FUNCTION(node) {
        if (!node->definition) continue;
        
        function* fn = DECL_STRUCT_FUNCTION(node->decl);
        if (!fn) continue;
        
        CUTIE_TRY_BEGIN(ctx);
        walk_function(ctx, fn);
        CUTIE_CHECK_ERROR(ctx);
        CUTIE_TRY_END(ctx);
    }
    
    CUTIE_DEBUG_INFO("optimization completed");
    return ;
}

// Main optimization pass
unsigned int execute_dynamic_array_scalarization(CutieContext* ctx) {
    execute_dynamic_array_scalarization_impl(ctx);
    return ctx->error_code;
}

// Pass structure
namespace {

const pass_data pass_data_dynamic_array_scalar = {
    GIMPLE_PASS,
    "dynamic_array_scalarization",
    OPTGROUP_NONE,
    TV_NONE,
    PROP_cfg,
    0,
    0,
    0,
    0
};

class pass_dynamic_array_scalar : public gimple_opt_pass {
    CutieContext* ctx;
    
public:
    pass_dynamic_array_scalar(gcc::context* ctxt, CutieContext* context)
        : gimple_opt_pass(pass_data_dynamic_array_scalar, ctxt), ctx(context) {}
    
    virtual unsigned int execute(function*) override {
        return execute_dynamic_array_scalarization(ctx);
    }
    
    virtual bool gate(function*) override {
        return flag_lto && optimize > 0;
    }
};

} // namespace

// Plugin initialization
int plugin_init(struct plugin_name_args* plugin_info, struct plugin_gcc_version* version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        printf("(%s:%d %s [version mismatch])\n", __FILE__, __LINE__, __FUNCTION__);
        return 1;
    }
    
    // Allocate and initialize context
    static CutieContext ctx_storage;
    CutieContext* ctx = &ctx_storage;
    init(ctx);
    
    // CUTIE_CHECK_ERROR(ctx);
    
    // Register pass
    struct register_pass_info pass_info;
    pass_info.pass = new pass_dynamic_array_scalar(g, ctx);
    pass_info.reference_pass_name = "early_optimizations";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    
    CUTIE_DEBUG_INFO("plugin initialized successfully");
    
    return 0;
}
