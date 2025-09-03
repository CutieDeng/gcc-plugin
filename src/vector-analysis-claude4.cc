#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "tree-pass.h"
#include "context.h"
#include "function.h"
#include "basic-block.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "cgraph.h"
#include "tree-cfg.h"
#include "tree-ssa.h"
#include "hash-map.h"
#include "vec.h"

int plugin_is_GPL_compatible;

// 错误码定义
using err_t = long long;

enum error_type : err_t {
    ErrorOk = 0,
    ErrorWeakAssert = 1,
    ErrorStrongAssert = 2,
    ErrorGccLogic = 4,
    ErrorCustomLogic = 8,
    ErrorMemResource = 16,
    ErrorIo = 32
};

// 分析结果枚举
enum analysis_result {
    RESULT_SUPPORT = 1,
    RESULT_OPPOSE = 2,
    RESULT_IRRELEVANT = 3
};

// 模式代码宏定义
#define PATTERN_BEGIN err_t ret = ErrorOk;

#define PATTERN_SAFE_CHECK_STRONG() do { \
    if (ret != ErrorOk) { \
        return ret; \
    } \
} while (0)

#define PATTERN_SAFE_CHECK_WEAK() do { \
    if (ret == ErrorStrongAssert || ret == ErrorGccLogic || ret == ErrorMemResource) { \
        return ret; \
    } \
    ret = ErrorOk; \
} while (0)

#define PATTERN_MATCH(x, y, e) do { \
    if ((x) == 0) { \
        (y) = (e); \
    } else if ((x) != (e)) { \
        ret = ErrorStrongAssert; \
        DEBUG("Pattern match failed: expected %lld, got %lld", (long long)(e), (long long)(x)); \
        return ret; \
    } \
} while (0)

#define PATTERN_MATCH_WEAK(x, y, e) do { \
    if ((x) == 0) { \
        (y) = (e); \
    } else if ((x) != (e)) { \
        ret = ErrorWeakAssert; \
        DEBUG("Weak pattern match failed: expected %lld, got %lld", (long long)(e), (long long)(x)); \
    } \
} while (0)

#define PATTERN_MATCH_STRONG(x, y, e) do { \
    if ((x) == 0) { \
        (y) = (e); \
    } else if ((x) != (e)) { \
        ret = ErrorStrongAssert; \
        DEBUG("Strong pattern match failed: expected %lld, got %lld", (long long)(e), (long long)(x)); \
        return ret; \
    } \
} while (0)

#define PATTERN_END return ret;

#define DEBUG(fmt, ...) do { \
    printf("[DEBUG] %s:%d %s: " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
} while (0)

// 三元组结构体
struct vector_triplet {
    tree data_ptr;
    tree size;
    tree capacity;
    char const *data_name;
    char const *size_name;
    char const *capacity_name;
};

// 类型分析信息
struct type_analysis_info {
    tree type_decl;
    vector_triplet triplet;
    bool is_vector_like;
    char const *type_name;
};

// 函数分析信息
struct function_analysis_info {
    tree function_decl;
    analysis_result result;
    char const *evidence;
    char const *function_name;
};

// 主分析器类
struct VectorAnalysis {
    hash_map<tree, type_analysis_info*> *type_info_map;
    auto_vec<function_analysis_info> *function_results;
    auto_vec<type_analysis_info> *type_results;
    bool initialized;
    
    err_t init();
    err_t analyze_program();
    err_t analyze_types();
    err_t analyze_functions();
    err_t analyze_single_type(tree type_decl, type_analysis_info &info);
    err_t analyze_single_function(cgraph_node *node, function_analysis_info &info);
    err_t find_triplet_in_type(tree type_decl, vector_triplet &triplet);
    err_t analyze_function_operations(tree function_decl, vector_triplet const &triplet, analysis_result &result);
    err_t check_pointer_field(tree field, bool &is_pointer);
    err_t check_size_field(tree field, bool &is_size_like);
    err_t check_field_name_pattern(tree field, bool &is_data_ptr, bool &is_size, bool &is_capacity);
    err_t output_results();
    err_t cleanup();
};

err_t VectorAnalysis::init() {
    PATTERN_BEGIN
    
    DEBUG("Initializing VectorAnalysis");
    
    if (initialized) {
        ret = ErrorWeakAssert;
        DEBUG("Already initialized");
        PATTERN_END
    }
    
    type_info_map = new hash_map<tree, type_analysis_info*>();
    if (!type_info_map) {
        ret = ErrorMemResource;
        DEBUG("Failed to allocate type_info_map");
        PATTERN_END
    }
    
    function_results = new auto_vec<function_analysis_info>();
    if (!function_results) {
        ret = ErrorMemResource;
        DEBUG("Failed to allocate function_results");
        delete type_info_map;
        PATTERN_END
    }
    
    type_results = new auto_vec<type_analysis_info>();
    if (!type_results) {
        ret = ErrorMemResource;
        DEBUG("Failed to allocate type_results");
        delete type_info_map;
        delete function_results;
        PATTERN_END
    }
    
    initialized = true;
    DEBUG("VectorAnalysis initialized successfully");
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_program() {
    PATTERN_BEGIN
    
    DEBUG("Starting vector container analysis");
    
    if (!initialized) {
        ret = init();
        PATTERN_SAFE_CHECK_STRONG();
    }
    
    ret = analyze_types();
    PATTERN_SAFE_CHECK_STRONG();
    
    ret = analyze_functions();
    PATTERN_SAFE_CHECK_STRONG();
    
    ret = output_results();
    PATTERN_SAFE_CHECK_STRONG();
    
    DEBUG("Vector analysis completed successfully");
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_types() {
    PATTERN_BEGIN
    
    DEBUG("Analyzing types");
    
    if (!type_results) {
        ret = ErrorGccLogic;
        DEBUG("type_results is null");
        PATTERN_END
    }
    
    // 遍历所有定义的类型
    tree type_node;
    for (type_node = TYPE_MAIN_VARIANT(void_type_node); 
         type_node; 
         type_node = TYPE_NEXT_VARIANT(type_node)) {
        
        if (!TYPE_P(type_node) || !COMPLETE_TYPE_P(type_node)) {
            continue;
        }
        
        if (TREE_CODE(type_node) != RECORD_TYPE && TREE_CODE(type_node) != UNION_TYPE) {
            continue;
        }
        
        type_analysis_info info = {};
        ret = analyze_single_type(type_node, info);
        PATTERN_SAFE_CHECK_WEAK();
        
        if (info.is_vector_like && ret == ErrorOk) {
            type_results->safe_push(info);
            type_info_map->put(type_node, &type_results->last());
            DEBUG("Found vector-like type: %s", info.type_name ? info.type_name : "<anonymous>");
        }
    }
    
    DEBUG("Found %d vector-like types", type_results->length());
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_single_type(tree type_decl, type_analysis_info &info) {
    PATTERN_BEGIN
    
    if (!type_decl || !TYPE_P(type_decl)) {
        ret = ErrorWeakAssert;
        DEBUG("Invalid type_decl");
        PATTERN_END
    }
    
    info.type_decl = type_decl;
    info.is_vector_like = false;
    info.triplet = {};
    info.type_name = nullptr;
    
    if (TYPE_NAME(type_decl) && DECL_NAME(TYPE_NAME(type_decl))) {
        info.type_name = IDENTIFIER_POINTER(DECL_NAME(TYPE_NAME(type_decl)));
    }
    
    ret = find_triplet_in_type(type_decl, info.triplet);
    PATTERN_SAFE_CHECK_WEAK();
    
    // 检查是否找到了完整的三元组
    if (info.triplet.data_ptr && info.triplet.size && info.triplet.capacity) {
        info.is_vector_like = true;
        DEBUG("Type %s has complete vector triplet", info.type_name ? info.type_name : "<anonymous>");
    }
    
    PATTERN_END
}

err_t VectorAnalysis::find_triplet_in_type(tree type_decl, vector_triplet &triplet) {
    PATTERN_BEGIN
    
    if (TREE_CODE(type_decl) != RECORD_TYPE) {
        ret = ErrorWeakAssert;
        DEBUG("Not a record type");
        PATTERN_END
    }
    
    tree field = TYPE_FIELDS(type_decl);
    if (!field) {
        ret = ErrorWeakAssert;
        DEBUG("No fields found");
        PATTERN_END
    }
    
    triplet = {};
    int pointer_count = 0;
    int size_count = 0;
    
    for (; field; field = DECL_CHAIN(field)) {
        if (TREE_CODE(field) != FIELD_DECL) {
            continue;
        }
        
        bool is_pointer = false;
        bool is_size_like = false;
        bool is_data_ptr = false;
        bool is_size = false;
        bool is_capacity = false;
        
        err_t check_ret = check_pointer_field(field, is_pointer);
        PATTERN_SAFE_CHECK_WEAK();
        
        check_ret = check_size_field(field, is_size_like);
        PATTERN_SAFE_CHECK_WEAK();
        
        check_ret = check_field_name_pattern(field, is_data_ptr, is_size, is_capacity);
        PATTERN_SAFE_CHECK_WEAK();
        
        // 基于字段类型和名称模式匹配三元组成员
        if (is_pointer && (is_data_ptr || !triplet.data_ptr)) {
            triplet.data_ptr = field;
            if (DECL_NAME(field)) {
                triplet.data_name = IDENTIFIER_POINTER(DECL_NAME(field));
            }
            pointer_count++;
        } else if (is_size_like) {
            if (is_size && !triplet.size) {
                triplet.size = field;
                if (DECL_NAME(field)) {
                    triplet.size_name = IDENTIFIER_POINTER(DECL_NAME(field));
                }
                size_count++;
            } else if (is_capacity && !triplet.capacity) {
                triplet.capacity = field;
                if (DECL_NAME(field)) {
                    triplet.capacity_name = IDENTIFIER_POINTER(DECL_NAME(field));
                }
                size_count++;
            } else if (!is_size && !is_capacity) {
                // 通用整数字段，按顺序分配
                if (!triplet.size) {
                    triplet.size = field;
                    if (DECL_NAME(field)) {
                        triplet.size_name = IDENTIFIER_POINTER(DECL_NAME(field));
                    }
                    size_count++;
                } else if (!triplet.capacity) {
                    triplet.capacity = field;
                    if (DECL_NAME(field)) {
                        triplet.capacity_name = IDENTIFIER_POINTER(DECL_NAME(field));
                    }
                    size_count++;
                }
            }
        }
    }
    
    // 验证找到的三元组是否合理
    if (pointer_count >= 1 && size_count >= 2) {
        DEBUG("Found potential vector triplet: ptr=%s, size=%s, cap=%s",
              triplet.data_name ? triplet.data_name : "<unnamed>",
              triplet.size_name ? triplet.size_name : "<unnamed>",
              triplet.capacity_name ? triplet.capacity_name : "<unnamed>");
        ret = ErrorOk;
    } else {
        ret = ErrorWeakAssert;
        DEBUG("Insufficient fields for vector pattern: ptr_count=%d, size_count=%d", 
              pointer_count, size_count);
    }
    
    PATTERN_END
}

err_t VectorAnalysis::check_pointer_field(tree field, bool &is_pointer) {
    PATTERN_BEGIN
    
    is_pointer = false;
    
    if (!field || TREE_CODE(field) != FIELD_DECL) {
        ret = ErrorWeakAssert;
        PATTERN_END
    }
    
    tree field_type = TREE_TYPE(field);
    if (!field_type) {
        ret = ErrorWeakAssert;
        PATTERN_END
    }
    
    if (TREE_CODE(field_type) == POINTER_TYPE) {
        is_pointer = true;
        DEBUG("Found pointer field: %s", 
              DECL_NAME(field) ? IDENTIFIER_POINTER(DECL_NAME(field)) : "<unnamed>");
    }
    
    PATTERN_END
}

err_t VectorAnalysis::check_size_field(tree field, bool &is_size_like) {
    PATTERN_BEGIN
    
    is_size_like = false;
    
    if (!field || TREE_CODE(field) != FIELD_DECL) {
        ret = ErrorWeakAssert;
        PATTERN_END
    }
    
    tree field_type = TREE_TYPE(field);
    if (!field_type) {
        ret = ErrorWeakAssert;
        PATTERN_END
    }
    
    if (INTEGRAL_TYPE_P(field_type) && TYPE_UNSIGNED(field_type)) {
        is_size_like = true;
        DEBUG("Found size-like field: %s", 
              DECL_NAME(field) ? IDENTIFIER_POINTER(DECL_NAME(field)) : "<unnamed>");
    }
    
    PATTERN_END
}

err_t VectorAnalysis::check_field_name_pattern(tree field, bool &is_data_ptr, bool &is_size, bool &is_capacity) {
    PATTERN_BEGIN
    
    is_data_ptr = false;
    is_size = false;
    is_capacity = false;
    
    if (!field || !DECL_NAME(field)) {
        ret = ErrorOk; // 没有名称不是错误，只是无法通过名称判断
        PATTERN_END
    }
    
    char const *name = IDENTIFIER_POINTER(DECL_NAME(field));
    if (!name) {
        ret = ErrorOk;
        PATTERN_END
    }
    
    // 检查数据指针模式
    if (strstr(name, "data") || strstr(name, "ptr") || strstr(name, "begin") || 
        strstr(name, "start") || strstr(name, "_M_impl")) {
        is_data_ptr = true;
    }
    
    // 检查大小字段模式
    if (strstr(name, "size") || strstr(name, "length") || strstr(name, "len") ||
        strstr(name, "count") || strstr(name, "_M_finish")) {
        is_size = true;
    }
    
    // 检查容量字段模式
    if (strstr(name, "capacity") || strstr(name, "cap") || strstr(name, "max") ||
        strstr(name, "alloc") || strstr(name, "_M_end_of_storage")) {
        is_capacity = true;
    }
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_functions() {
    PATTERN_BEGIN
    
    DEBUG("Analyzing functions");
    
    if (!function_results) {
        ret = ErrorGccLogic;
        DEBUG("function_results is null");
        PATTERN_END
    }
    
    cgraph_node *node;
    FOR_EACH_FUNCTION(node) {
        if (!node->definition) {
            continue;
        }
        
        function_analysis_info info = {};
        ret = analyze_single_function(node, info);
        PATTERN_SAFE_CHECK_WEAK();
        
        if (ret == ErrorOk) {
            function_results->safe_push(info);
        }
    }
    
    DEBUG("Analyzed %d functions", function_results->length());
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_single_function(cgraph_node *node, function_analysis_info &info) {
    PATTERN_BEGIN
    
    if (!node || !node->definition) {
        ret = ErrorWeakAssert;
        DEBUG("Invalid cgraph node");
        PATTERN_END
    }
    
    tree function_decl = node->decl;
    if (!function_decl) {
        ret = ErrorWeakAssert;
        DEBUG("No function declaration");
        PATTERN_END
    }
    
    info.function_decl = function_decl;
    info.result = RESULT_IRRELEVANT;
    info.evidence = "No vector operations detected";
    info.function_name = nullptr;
    
    if (DECL_NAME(function_decl)) {
        info.function_name = IDENTIFIER_POINTER(DECL_NAME(function_decl));
    }
    
    // 检查函数是否操作vector-like类型
    bool found_vector_operation = false;
    
    for (unsigned i = 0; i < type_results->length(); i++) {
        type_analysis_info &type_info = (*type_results)[i];
        
        analysis_result op_result;
        ret = analyze_function_operations(function_decl, type_info.triplet, op_result);
        PATTERN_SAFE_CHECK_WEAK();
        
        if (op_result != RESULT_IRRELEVANT) {
            found_vector_operation = true;
            info.result = op_result;
            
            if (op_result == RESULT_SUPPORT) {
                info.evidence = "Function supports vector semantics";
            } else if (op_result == RESULT_OPPOSE) {
                info.evidence = "Function violates vector semantics";
            }
            break;
        }
    }
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_function_operations(tree function_decl, vector_triplet const &triplet, analysis_result &result) {
    PATTERN_BEGIN
    
    result = RESULT_IRRELEVANT;
    
    if (!function_decl) {
        ret = ErrorWeakAssert;
        DEBUG("Invalid function declaration");
        PATTERN_END
    }
    
    function *func = DECL_STRUCT_FUNCTION(function_decl);
    if (!func) {
        ret = ErrorWeakAssert;
        DEBUG("No function structure");
        PATTERN_END
    }
    
    bool has_size_update = false;
    bool has_capacity_check = false;
    bool has_reallocation = false;
    bool has_boundary_check = false;
    bool has_invalid_access = false;
    
    basic_block bb;
    FOR_EACH_BB_FN(bb, func) {
        gimple_stmt_iterator gsi;
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            
            if (gimple_code(stmt) == GIMPLE_ASSIGN) {
                tree lhs = gimple_assign_lhs(stmt);
                tree rhs1 = gimple_assign_rhs1(stmt);
                
                // 检查size/capacity更新
                if (lhs && (lhs == triplet.size || lhs == triplet.capacity)) {
                    has_size_update = true;
                }
                
                // 检查容量检查
                if (rhs1 && (rhs1 == triplet.capacity || rhs1 == triplet.size)) {
                    has_capacity_check = true;
                }
                
                // 检查边界检查模式
                if (gimple_assign_rhs_code(stmt) == LT_EXPR || 
                    gimple_assign_rhs_code(stmt) == LE_EXPR ||
                    gimple_assign_rhs_code(stmt) == GT_EXPR ||
                    gimple_assign_rhs_code(stmt) == GE_EXPR) {
                    tree rhs2 = gimple_assign_rhs2(stmt);
                    if ((rhs1 == triplet.size && rhs2 == triplet.capacity) ||
                        (rhs2 == triplet.size && rhs1 == triplet.capacity)) {
                        has_boundary_check = true;
                    }
                }
            }
            
            if (gimple_code(stmt) == GIMPLE_CALL) {
                tree fndecl = gimple_call_fndecl(stmt);
                if (fndecl && DECL_NAME(fndecl)) {
                    char const *name = IDENTIFIER_POINTER(DECL_NAME(fndecl));
                    if (name && (strstr(name, "alloc") || strstr(name, "realloc") || 
                                strstr(name, "free") || strstr(name, "malloc"))) {
                        has_reallocation = true;
                    }
                }
            }
            
            // 检查可能的无效访问
            if (gimple_code(stmt) == GIMPLE_ASSIGN) {
                tree rhs1 = gimple_assign_rhs1(stmt);
                if (rhs1 && TREE_CODE(rhs1) == MEM_REF) {
                    tree base = TREE_OPERAND(rhs1, 0);
                    if (base == triplet.data_ptr) {
                        // 这里可以进一步检查是否有越界访问
                        // 简化处理：如果没有边界检查就认为可能有问题
                        if (!has_boundary_check) {
                            has_invalid_access = true;
                        }
                    }
                }
            }
        }
    }
    
    // 基于模式匹配结果判断
    if (has_size_update && has_capacity_check && has_boundary_check) {
        result = RESULT_SUPPORT;
    } else if (has_invalid_access || (has_reallocation && !has_capacity_check)) {
        result = RESULT_OPPOSE;
    } else if (has_size_update || has_capacity_check || has_reallocation) {
        result = RESULT_SUPPORT;
    }
    
    PATTERN_END
}

err_t VectorAnalysis::output_results() {
    PATTERN_BEGIN
    
    printf("\n========== Vector Container Analysis Results ==========\n");
    
    printf("\n--- Vector-like Types Found: %d ---\n", type_results->length());
    for (unsigned i = 0; i < type_results->length(); i++) {
        type_analysis_info &info = (*type_results)[i];
        printf("  [%d] Type: %s\n", i + 1, info.type_name ? info.type_name : "<anonymous>");
        printf("      - Data pointer: %s\n", info.triplet.data_name ? info.triplet.data_name : "<unnamed>");
        printf("      - Size field: %s\n", info.triplet.size_name ? info.triplet.size_name : "<unnamed>");
        printf("      - Capacity field: %s\n", info.triplet.capacity_name ? info.triplet.capacity_name : "<unnamed>");
    }
    
    printf("\n--- Function Analysis Results: %d ---\n", function_results->length());
    int support_count = 0;
    int oppose_count = 0;
    int irrelevant_count = 0;
    
    for (unsigned i = 0; i < function_results->length(); i++) {
        function_analysis_info &info = (*function_results)[i];
        char const *result_str = "";
        
        switch (info.result) {
            case RESULT_SUPPORT:
                result_str = "SUPPORT";
                support_count++;
                break;
            case RESULT_OPPOSE:
                result_str = "OPPOSE";
                oppose_count++;
                break;
            case RESULT_IRRELEVANT:
                result_str = "IRRELEVANT";
                irrelevant_count++;
                break;
        }
        
        if (info.result != RESULT_IRRELEVANT) {
            printf("  [%d] Function: %s\n", i + 1, info.function_name ? info.function_name : "<anonymous>");
            printf("      - Result: %s\n", result_str);
            printf("      - Evidence: %s\n", info.evidence);
        }
    }
    
    printf("\n--- Summary ---\n");
    printf("  Support: %d functions\n", support_count);
    printf("  Oppose: %d functions\n", oppose_count);
    printf("  Irrelevant: %d functions\n", irrelevant_count);
    printf("  Vector-like types detected: %d\n", type_results->length());
    
    printf("\n================== Analysis Complete ==================\n");
    
    PATTERN_END
}

err_t VectorAnalysis::cleanup() {
    PATTERN_BEGIN
    
    DEBUG("Cleaning up VectorAnalysis");
    
    if (type_info_map) {
        delete type_info_map;
        type_info_map = nullptr;
    }
    
    if (function_results) {
        delete function_results;
        function_results = nullptr;
    }
    
    if (type_results) {
        delete type_results;
        type_results = nullptr;
    }
    
    initialized = false;
    
    PATTERN_END
}

// IPA Pass实现
namespace {
    const pass_data vector_analyzer_pass_data = {
        IPA_PASS,
        "vector_analyzer",
        OPTGROUP_NONE,
        TV_IPA_FREE_LANG_DATA,
        0,
        0,
        0,
        0,
        0
    };

    class vector_analyzer_pass : public ipa_opt_pass_d {
    private:
        VectorAnalysis analysis;
        
    public:
        vector_analyzer_pass(gcc::context *ctxt)
            : ipa_opt_pass_d(vector_analyzer_pass_data, ctxt,
                           NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL) {
            analysis.initialized = false;
            analysis.type_info_map = nullptr;
            analysis.function_results = nullptr;
            analysis.type_results = nullptr;
        }

        virtual unsigned int execute(function*) override {
            err_t ret = analysis.analyze_program();
            if (ret != ErrorOk) {
                DEBUG("Vector analysis failed with error code: %lld", ret);
            }
            analysis.cleanup();
            return 0;
        }
    };
}

// 插件初始化
int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version) {
    
    if (!plugin_default_version_check(version, &gcc_version)) {
        printf("ERROR: Plugin version check failed\n");
        return 1;
    }

    struct register_pass_info pass_info;
    pass_info.pass = new vector_analyzer_pass(g);
    pass_info.reference_pass_name = "whole-program";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    
    printf("Vector analyzer plugin initialized successfully\n");
    return 0;
}
