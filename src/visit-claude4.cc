#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <gimple-walk.h>
#include <tree-ssa-alias.h>
#include <tree-cfg.h>
#include <basic-block.h>
#include <function.h>
#include <cgraph.h>
#include <tree-pretty-print.h>
#include <print-tree.h>
#include <internal-fn.h>
#include <gimple-pretty-print.h>
#include <context.h>

/* Plugin licensing */
int plugin_is_GPL_compatible;

/* Error code definitions using bit fields */
enum error_type {
    ERR_SUCCESS = 0,
    ERR_FRAMEWORK_LOGIC = 1 << 0,    /* Framework assertion failure */
    ERR_WEAK_ASSERTION = 1 << 1,     /* Weak assertion failure - safe to continue */
    ERR_LOGIC = 1 << 2,              /* Custom logic error */
    ERR_MEMORY = 1 << 3,             /* Memory allocation failure */
    ERR_INVALID_INPUT = 1 << 4,      /* Invalid input parameters */
    ERR_NOT_FOUND = 1 << 5           /* Element not found */
};

/* Debug macros for error handling and tracing */
#define DEBUG_TRACE(msg, ...) \
    do { \
        printf("[TRACE:%s:%d] " msg "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#define TRY(call) \
    do { \
        long long __result = (call); \
        if (__result != ERR_SUCCESS) { \
            DEBUG_TRACE("Error %lld returned from: " #call, __result); \
            return __result; \
        } \
    } while (0)

#define TRY_WEAK(call) \
    do { \
        long long __result = (call); \
        if (__result != ERR_SUCCESS && !(__result & ERR_WEAK_ASSERTION)) { \
            DEBUG_TRACE("Error %lld returned from: " #call, __result); \
            return __result; \
        } \
    } while (0)

/* Access type enumeration */
enum access_type {
    ACCESS_READ = 0,
    ACCESS_WRITE = 1
};

/* Pattern type enumeration */
enum pattern_type {
    PATTERN_DIRECT_ARRAY,      /* Direct array access: arr[i] */
    PATTERN_POINTER_DEREF,     /* Pointer dereference: *ptr */
    PATTERN_MEMBER_ARRAY,      /* Member array access: obj.arr[i] */
    PATTERN_MEMBER_POINTER,    /* Member pointer access: obj.ptr */
    PATTERN_INDIRECT_ARRAY,    /* Indirect array access: obj.ptr[i] */
    PATTERN_UNKNOWN
};

/* Data structure to represent collected access patterns */
struct access_tuple {
    tree struct_type;           /* The structure type (if applicable) */
    tree field_decl;           /* The field declaration (if applicable) */
    tree pointed_element_type;  /* Type of the pointed element */
    tree base_expr;            /* Base expression */
    tree access_expr;          /* Full access expression */
    gimple *stmt;              /* Statement containing the access */
    enum access_type access_mode; /* Read or write access */
    enum pattern_type pattern;  /* Type of access pattern */
    
    /* Default constructor */
    access_tuple() : struct_type(NULL), field_decl(NULL), pointed_element_type(NULL),
                    base_expr(NULL), access_expr(NULL), stmt(NULL), 
                    access_mode(ACCESS_READ), pattern(PATTERN_UNKNOWN) {}
};

/* Main analyzer class - designed with trivial lifecycle */
class pointer_access_analyzer {
private:
    /* Container for collected access tuples */
    auto_vec<access_tuple> collected_accesses;
    
    /* Statistics */
    unsigned int total_functions_analyzed;
    unsigned int total_statements_analyzed;
    unsigned int total_array_accesses;
    unsigned int total_pointer_accesses;
    
public:
    /* Constructor */
    pointer_access_analyzer() : total_functions_analyzed(0), total_statements_analyzed(0),
                               total_array_accesses(0), total_pointer_accesses(0) {
        DEBUG_TRACE("Analyzer created");
    }
    
    /* Destructor */
    ~pointer_access_analyzer() {
        DEBUG_TRACE("Analyzer destroyed, analyzed %u functions, %u statements, found %u array accesses, %u pointer accesses", 
                   total_functions_analyzed, total_statements_analyzed, 
                   total_array_accesses, total_pointer_accesses);
    }
    
    /* Main analysis entry point */
    long long analyze_compilation_unit();
    
    /* Print all collected results */
    void print_results() const;
    
private:
    /* Core analysis methods */
    long long analyze_function(struct cgraph_node *node);
    long long analyze_basic_block(basic_block bb);
    long long analyze_gimple_stmt(gimple *stmt);
    
    /* Enhanced pattern detection methods */
    long long analyze_expression_tree(tree expr, gimple *stmt, enum access_type access_mode);
    long long analyze_array_access(tree expr, gimple *stmt, enum access_type access_mode);
    long long analyze_pointer_access(tree expr, gimple *stmt, enum access_type access_mode);
    long long analyze_component_ref(tree expr, gimple *stmt, enum access_type access_mode);
    long long analyze_mem_ref(tree expr, gimple *stmt, enum access_type access_mode);
    
    /* Utility methods */
    const char *access_type_to_string(enum access_type type) const;
    const char *pattern_type_to_string(enum pattern_type type) const;
    void print_access_tuple(const access_tuple *tuple) const;
    void print_tree_debug(tree expr, const char *prefix) const;
    long long add_access_tuple(const access_tuple &tuple);
    bool is_struct_type(tree type) const;
    bool is_pointer_type(tree type) const;
    bool is_array_type(tree type) const;
};

/* Main analysis entry point */
long long pointer_access_analyzer::analyze_compilation_unit()
{
    DEBUG_TRACE("Starting pointer access analysis");
    
    struct cgraph_node *node;
    
    /* Iterate through all functions with GIMPLE body in the compilation unit */
    FOR_EACH_FUNCTION_WITH_GIMPLE_BODY(node) {
        TRY(analyze_function(node));
    }
    
    DEBUG_TRACE("Analysis completed. Functions: %u, Statements: %u", 
               total_functions_analyzed, total_statements_analyzed);
    
    return ERR_SUCCESS;
}

/* Analyze a single function */
long long pointer_access_analyzer::analyze_function(struct cgraph_node *node)
{
    if (!node) {
        return ERR_INVALID_INPUT;
    }
    
    struct function *func = DECL_STRUCT_FUNCTION(node->decl);
    if (!func) {
        return ERR_WEAK_ASSERTION;
    }
    
    const char *func_name = IDENTIFIER_POINTER(DECL_NAME(node->decl));
    DEBUG_TRACE("Analyzing function: %s", func_name ? func_name : "<unnamed>");
    
    total_functions_analyzed++;
    
    basic_block bb;
    FOR_EACH_BB_FN(bb, func) {
        TRY(analyze_basic_block(bb));
    }
    
    return ERR_SUCCESS;
}

/* Analyze a basic block */
long long pointer_access_analyzer::analyze_basic_block(basic_block bb)
{
    if (!bb) {
        return ERR_INVALID_INPUT;
    }
    
    gimple_stmt_iterator gsi;
    for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
        gimple *stmt = gsi_stmt(gsi);
        if (!stmt) continue;
        
        total_statements_analyzed++;
        
        long long result = analyze_gimple_stmt(stmt);
        if (result != ERR_SUCCESS && !(result & ERR_WEAK_ASSERTION)) {
            DEBUG_TRACE("Error analyzing statement: %lld", result);
        }
    }
    
    return ERR_SUCCESS;
}

/* Analyze a single GIMPLE statement */
long long pointer_access_analyzer::analyze_gimple_stmt(gimple *stmt)
{
    if (!stmt) {
        return ERR_INVALID_INPUT;
    }

    /* Debug: print the statement */
    DEBUG_TRACE("Analyzing statement:");
    print_gimple_stmt(stdout, stmt, 0, TDF_SLIM);

    switch (gimple_code(stmt)) {
        case GIMPLE_ASSIGN: {
            tree lhs = gimple_assign_lhs(stmt);
            tree rhs = gimple_assign_rhs1(stmt);
            
            /* Analyze LHS (write access) */
            if (lhs) {
                TRY_WEAK(analyze_expression_tree(lhs, stmt, ACCESS_WRITE));
            }
            
            /* Analyze RHS (read access) */
            if (rhs) {
                TRY_WEAK(analyze_expression_tree(rhs, stmt, ACCESS_READ));
            }
            
            /* Handle multi-operand assignments */
            if (gimple_num_ops(stmt) > 2) {
                for (unsigned i = 2; i < gimple_num_ops(stmt); i++) {
                    tree op = gimple_op(stmt, i);
                    if (op) {
                        TRY_WEAK(analyze_expression_tree(op, stmt, ACCESS_READ));
                    }
                }
            }
            break;
        }
        case GIMPLE_CALL: {
            /* Analyze function call arguments */
            unsigned num_args = gimple_call_num_args(stmt);
            for (unsigned i = 0; i < num_args; i++) {
                tree arg = gimple_call_arg(stmt, i);
                if (arg) {
                    TRY_WEAK(analyze_expression_tree(arg, stmt, ACCESS_READ));
                }
            }
            
            /* Analyze return value if assigned */
            tree lhs = gimple_call_lhs(stmt);
            if (lhs) {
                TRY_WEAK(analyze_expression_tree(lhs, stmt, ACCESS_WRITE));
            }
            break;
        }
        case GIMPLE_RETURN: {
            tree retval = gimple_return_retval(stmt);
            if (retval) {
                TRY_WEAK(analyze_expression_tree(retval, stmt, ACCESS_READ));
            }
            break;
        }
        default:
            /* Other statement types might also contain relevant expressions */
            break;
    }

    return ERR_SUCCESS;
}

/* Enhanced expression tree analysis */
long long pointer_access_analyzer::analyze_expression_tree(tree expr, gimple *stmt, enum access_type access_mode)
{
    if (!expr) {
        return ERR_INVALID_INPUT;
    }

    /* Debug output */
    DEBUG_TRACE("Analyzing expression tree (access_mode: %s):", access_type_to_string(access_mode));
    print_tree_debug(expr, "  ");

    enum tree_code code = TREE_CODE(expr);
    
    switch (code) {
        case ARRAY_REF:
            TRY_WEAK(analyze_array_access(expr, stmt, access_mode));
            break;
            
        case MEM_REF:
        case INDIRECT_REF:
            TRY_WEAK(analyze_mem_ref(expr, stmt, access_mode));
            break;
            
        case COMPONENT_REF:
            TRY_WEAK(analyze_component_ref(expr, stmt, access_mode));
            break;
            
        case POINTER_PLUS_EXPR:
        case PLUS_EXPR:
        case MINUS_EXPR:
            /* Analyze operands of arithmetic expressions */
            for (int i = 0; i < TREE_OPERAND_LENGTH(expr); i++) {
                tree operand = TREE_OPERAND(expr, i);
                if (operand) {
                    TRY_WEAK(analyze_expression_tree(operand, stmt, access_mode));
                }
            }
            break;
            
        case VAR_DECL:
        case PARM_DECL:
            /* Check if this is a pointer or array variable */
            if (is_pointer_type(TREE_TYPE(expr)) || is_array_type(TREE_TYPE(expr))) {
                TRY_WEAK(analyze_pointer_access(expr, stmt, access_mode));
            }
            break;
            
        default:
            /* For other expression types, recursively analyze operands */
            for (int i = 0; i < TREE_OPERAND_LENGTH(expr); i++) {
                tree operand = TREE_OPERAND(expr, i);
                if (operand) {
                    TRY_WEAK(analyze_expression_tree(operand, stmt, access_mode));
                }
            }
            break;
    }
    
    return ERR_SUCCESS;
}

/* Analyze array access patterns */
long long pointer_access_analyzer::analyze_array_access(tree expr, gimple *stmt, enum access_type access_mode)
{
    if (!expr || TREE_CODE(expr) != ARRAY_REF) {
        return ERR_INVALID_INPUT;
    }

    DEBUG_TRACE("Found ARRAY_REF pattern");
    total_array_accesses++;

    tree array_base = TREE_OPERAND(expr, 0);
    tree array_index = TREE_OPERAND(expr, 1);
    
    access_tuple tuple;
    tuple.access_expr = expr;
    tuple.base_expr = array_base;
    tuple.stmt = stmt;
    tuple.access_mode = access_mode;
    tuple.pattern = PATTERN_DIRECT_ARRAY;
    tuple.pointed_element_type = TREE_TYPE(expr);
    
    /* Check if the array base involves a component reference */
    if (array_base && TREE_CODE(array_base) == COMPONENT_REF) {
        tree field = TREE_OPERAND(array_base, 1);
        tree object = TREE_OPERAND(array_base, 0);
        
        tuple.field_decl = field;
        tuple.struct_type = TREE_TYPE(object);
        tuple.pattern = PATTERN_MEMBER_ARRAY;
        
        DEBUG_TRACE("Array access through struct member");
    }
    /* Check if the array base is a pointer dereference */
    else if (array_base && (TREE_CODE(array_base) == MEM_REF || TREE_CODE(array_base) == INDIRECT_REF)) {
        tuple.pattern = PATTERN_INDIRECT_ARRAY;
        DEBUG_TRACE("Indirect array access through pointer");
        
        /* Analyze the pointer expression */
        tree pointer_expr = TREE_OPERAND(array_base, 0);
        if (pointer_expr && TREE_CODE(pointer_expr) == COMPONENT_REF) {
            tree field = TREE_OPERAND(pointer_expr, 1);
            tree object = TREE_OPERAND(pointer_expr, 0);
            
            tuple.field_decl = field;
            tuple.struct_type = TREE_TYPE(object);
            
            DEBUG_TRACE("Indirect array access through struct member pointer");
        }
    }
    
    TRY(add_access_tuple(tuple));
    
    /* Recursively analyze the array base and index */
    TRY_WEAK(analyze_expression_tree(array_base, stmt, ACCESS_READ));
    TRY_WEAK(analyze_expression_tree(array_index, stmt, ACCESS_READ));
    
    return ERR_SUCCESS;
}

/* Analyze pointer access patterns */
long long pointer_access_analyzer::analyze_pointer_access(tree expr, gimple *stmt, enum access_type access_mode)
{
    if (!expr) {
        return ERR_INVALID_INPUT;
    }

    tree expr_type = TREE_TYPE(expr);
    if (!is_pointer_type(expr_type) && !is_array_type(expr_type)) {
        return ERR_WEAK_ASSERTION;
    }

    DEBUG_TRACE("Found pointer/array variable access");
    total_pointer_accesses++;

    access_tuple tuple;
    tuple.access_expr = expr;
    tuple.base_expr = expr;
    tuple.stmt = stmt;
    tuple.access_mode = access_mode;
    tuple.pattern = PATTERN_POINTER_DEREF;
    
    if (is_pointer_type(expr_type)) {
        tuple.pointed_element_type = TREE_TYPE(expr_type);
    } else if (is_array_type(expr_type)) {
        tuple.pointed_element_type = TREE_TYPE(expr_type);
        tuple.pattern = PATTERN_DIRECT_ARRAY;
    }
    
    TRY(add_access_tuple(tuple));
    
    return ERR_SUCCESS;
}

/* Analyze component reference (struct member access) */
long long pointer_access_analyzer::analyze_component_ref(tree expr, gimple *stmt, enum access_type access_mode)
{
    if (!expr || TREE_CODE(expr) != COMPONENT_REF) {
        return ERR_INVALID_INPUT;
    }

    DEBUG_TRACE("Found COMPONENT_REF pattern");

    tree field = TREE_OPERAND(expr, 1);
    tree object = TREE_OPERAND(expr, 0);
    
    tree field_type = TREE_TYPE(field);
    
    /* Check if this is accessing a pointer or array member */
    if (is_pointer_type(field_type) || is_array_type(field_type)) {
        access_tuple tuple;
        tuple.access_expr = expr;
        tuple.base_expr = object;
        tuple.field_decl = field;
        tuple.struct_type = TREE_TYPE(object);
        tuple.stmt = stmt;
        tuple.access_mode = access_mode;
        
        if (is_pointer_type(field_type)) {
            tuple.pattern = PATTERN_MEMBER_POINTER;
            tuple.pointed_element_type = TREE_TYPE(field_type);
        } else {
            tuple.pattern = PATTERN_MEMBER_ARRAY;
            tuple.pointed_element_type = TREE_TYPE(field_type);
        }
        
        TRY(add_access_tuple(tuple));
        DEBUG_TRACE("Struct member pointer/array access");
    }
    
    /* Recursively analyze the object */
    TRY_WEAK(analyze_expression_tree(object, stmt, ACCESS_READ));
    
    return ERR_SUCCESS;
}

/* Analyze memory reference (pointer dereference) */
long long pointer_access_analyzer::analyze_mem_ref(tree expr, gimple *stmt, enum access_type access_mode)
{
    if (!expr || (TREE_CODE(expr) != MEM_REF && TREE_CODE(expr) != INDIRECT_REF)) {
        return ERR_INVALID_INPUT;
    }

    DEBUG_TRACE("Found MEM_REF/INDIRECT_REF pattern");

    tree pointer_expr = TREE_OPERAND(expr, 0);
    
    access_tuple tuple;
    tuple.access_expr = expr;
    tuple.base_expr = pointer_expr;
    tuple.stmt = stmt;
    tuple.access_mode = access_mode;
    tuple.pattern = PATTERN_POINTER_DEREF;
    tuple.pointed_element_type = TREE_TYPE(expr);
    
    /* Check if the pointer is a component reference */
    if (pointer_expr && TREE_CODE(pointer_expr) == COMPONENT_REF) {
        tree field = TREE_OPERAND(pointer_expr, 1);
        tree object = TREE_OPERAND(pointer_expr, 0);
        
        tuple.field_decl = field;
        tuple.struct_type = TREE_TYPE(object);
        
        DEBUG_TRACE("Pointer dereference through struct member");
    }
    
    TRY(add_access_tuple(tuple));
    
    /* Recursively analyze the pointer expression */
    TRY_WEAK(analyze_expression_tree(pointer_expr, stmt, ACCESS_READ));
    
    return ERR_SUCCESS;
}

/* Add an access tuple to the collection */
long long pointer_access_analyzer::add_access_tuple(const access_tuple &tuple)
{
    collected_accesses.safe_push(tuple);
    return ERR_SUCCESS;
}

/* Type checking utilities */
bool pointer_access_analyzer::is_struct_type(tree type) const
{
    return type && (TREE_CODE(type) == RECORD_TYPE || TREE_CODE(type) == UNION_TYPE);
}

bool pointer_access_analyzer::is_pointer_type(tree type) const
{
    return type && TREE_CODE(type) == POINTER_TYPE;
}

bool pointer_access_analyzer::is_array_type(tree type) const
{
    return type && TREE_CODE(type) == ARRAY_TYPE;
}

/* Convert access type to string for output */
const char *pointer_access_analyzer::access_type_to_string(enum access_type type) const
{
    switch (type) {
        case ACCESS_READ: return "READ";
        case ACCESS_WRITE: return "WRITE";
        default: return "UNKNOWN";
    }
}

/* Convert pattern type to string for output */
const char *pointer_access_analyzer::pattern_type_to_string(enum pattern_type type) const
{
    switch (type) {
        case PATTERN_DIRECT_ARRAY: return "DIRECT_ARRAY";
        case PATTERN_POINTER_DEREF: return "POINTER_DEREF";
        case PATTERN_MEMBER_ARRAY: return "MEMBER_ARRAY";
        case PATTERN_MEMBER_POINTER: return "MEMBER_POINTER";
        case PATTERN_INDIRECT_ARRAY: return "INDIRECT_ARRAY";
        default: return "UNKNOWN";
    }
}

/* Debug print tree structure */
void pointer_access_analyzer::print_tree_debug(tree expr, const char *prefix) const
{
    if (!expr) {
        printf("%s<NULL>\n", prefix);
        return;
    }
    
    printf("%s", prefix);
    print_generic_expr(stdout, expr, TDF_SLIM);
    printf(" (%s)\n", get_tree_code_name(TREE_CODE(expr)));
}

/* Print collected access tuple information */
void pointer_access_analyzer::print_access_tuple(const access_tuple *tuple) const
{
    if (!tuple) return;

    printf("=== Access Tuple ===\n");
    printf("Pattern: %s\n", pattern_type_to_string(tuple->pattern));
    printf("Access Mode: %s\n", access_type_to_string(tuple->access_mode));
    
    if (tuple->struct_type) {
        printf("Struct Type: ");
        print_generic_expr(stdout, tuple->struct_type, TDF_SLIM);
        printf("\n");
    }
    
    if (tuple->field_decl) {
        printf("Field: ");
        print_generic_expr(stdout, tuple->field_decl, TDF_SLIM);
        printf("\n");
    }
    
    if (tuple->pointed_element_type) {
        printf("Element Type: ");
        print_generic_expr(stdout, tuple->pointed_element_type, TDF_SLIM);
        printf("\n");
    }
    
    if (tuple->access_expr) {
        printf("Access Expression: ");
        print_generic_expr(stdout, tuple->access_expr, TDF_SLIM);
        printf("\n");
    }
    
    if (tuple->base_expr) {
        printf("Base Expression: ");
        print_generic_expr(stdout, tuple->base_expr, TDF_SLIM);
        printf("\n");
    }
    
    if (tuple->stmt) {
        printf("Statement: ");
        print_gimple_stmt(stdout, tuple->stmt, 0, TDF_SLIM);
    }
    
    printf("====================\n\n");
}

/* Print all collected results */
void pointer_access_analyzer::print_results() const
{
    printf("\n=== POINTER ACCESS ANALYSIS RESULTS ===\n");
    printf("Total functions analyzed: %u\n", total_functions_analyzed);
    printf("Total statements analyzed: %u\n", total_statements_analyzed);
    printf("Total array accesses found: %u\n", total_array_accesses);
    printf("Total pointer accesses found: %u\n", total_pointer_accesses);
    printf("Total collected accesses: %u\n\n", collected_accesses.length());
    
    unsigned int i;
    access_tuple *tuple;
    FOR_EACH_VEC_ELT(collected_accesses, i, tuple) {
        print_access_tuple(tuple);
    }
    
    printf("=== END ANALYSIS RESULTS ===\n");
}

/* IPA pass execution function - creates analyzer with local scope */
static unsigned int execute_pointer_access_analysis(void)
{
    DEBUG_TRACE("Creating analyzer instance");
    
    /* Create analyzer instance with local scope - no global state */
    pointer_access_analyzer analyzer;
    
    /* Execute analysis */
    long long result = analyzer.analyze_compilation_unit();
    if (result != ERR_SUCCESS) {
        printf("Analysis failed with error code: %lld\n", result);
        return 1;
    }
    
    /* Print results */
    analyzer.print_results();
    
    DEBUG_TRACE("Analysis completed successfully");
    return 0;
}

/* Define the IPA pass structure */
namespace {
    const pass_data pass_data_pointer_access_analysis = {
        IPA_PASS,                           /* type */
        "pointer_access_analysis",          /* name */
        OPTGROUP_NONE,                      /* optinfo_flags */
        TV_NONE,                           /* tv_id */
        0,                                  /* properties_required */
        0,                                  /* properties_provided */
        0,                                  /* properties_destroyed */
        0,                                  /* todo_flags_start */
        0,                                  /* todo_flags_finish */
    };

    class pass_pointer_access_analysis : public ipa_opt_pass_d {
    public:
        pass_pointer_access_analysis(gcc::context *ctxt)
            : ipa_opt_pass_d(pass_data_pointer_access_analysis, ctxt,
                         NULL,  // generate_summary
                         NULL,  // write_summary
                         NULL,  // read_summary
                         NULL,  // write_optimization_summary
                         NULL,  // read_optimization_summary
                         NULL,  // stmt_fixup
                         0,     // function_transform_todo_flags_start
                         NULL,  // function_transform
                         NULL)  // variable_transform
        {}

        virtual unsigned int execute(function *) override {
            return execute_pointer_access_analysis();
        }
    };
}

/* Plugin initialization */
int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version)
{
    /* Check GCC version compatibility */
    if (!plugin_default_version_check(version, &gcc_version)) {
        printf("Plugin version check failed\n");
        return 1;
    }

    /* Register the pass */
    struct register_pass_info pass_info = {
        .pass = new pass_pointer_access_analysis(g),
        .reference_pass_name = "whole-program",
        .ref_pass_instance_number = 1,
        .pos_op = PASS_POS_INSERT_AFTER
    };

    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP,
                     NULL, &pass_info);

    printf("Pointer access analysis plugin initialized successfully\n");
    return 0;
}
