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
#define ERR_FRAMEWORK    1
#define ERR_WEAK_ASSERT  2  
#define ERR_LOGIC        4
#define ERR_MEMORY       8

// 错误处理宏
#define TRY(expr) do { \
    long long ret = (expr); \
    if (ret != 0) { \
        printf("TRY failed at %s:%d, error=%lld\n", __FILE__, __LINE__, ret); \
        return ret; \
    } \
} while(0)

#define TRY_WEAK(expr) do { \
    long long ret = (expr); \
    if (ret != 0 && !(ret & ERR_WEAK_ASSERT)) { \
        printf("TRY_WEAK failed at %s:%d, error=%lld\n", __FILE__, __LINE__, ret); \
        return ret; \
    } \
} while(0)

// 分析结果枚举
enum analysis_result {
    RESULT_SUPPORT = 1,
    RESULT_OPPOSE = 2, 
    RESULT_IRRELEVANT = 4
};

// 三元组结构
struct vector_triplet {
    tree data_ptr;
    tree size;
    tree capacity;
    
    vector_triplet() : data_ptr(NULL_TREE), size(NULL_TREE), capacity(NULL_TREE) {}
};

// 类型分析信息
struct type_analysis_info {
    tree type_decl;
    vector_triplet triplet;
    bool is_vector_like;
    
    type_analysis_info() : type_decl(NULL_TREE), is_vector_like(false) {}
};

// 函数分析信息
struct function_analysis_info {
    tree function_decl;
    analysis_result result;
    const char* evidence;
    
    function_analysis_info() : function_decl(NULL_TREE), result(RESULT_IRRELEVANT), evidence("") {}
};

// 主分析器类
class VectorAnalyzer {
private:
    hash_map<tree, type_analysis_info*> type_info_map;
    auto_vec<function_analysis_info> function_results;
    auto_vec<type_analysis_info> type_results;

public:
    VectorAnalyzer() {}
    ~VectorAnalyzer() {
        cleanup_resources();
    }
    
    long long analyze_program();
    long long analyze_types();
    long long analyze_functions();
    long long analyze_single_type(tree type_decl, type_analysis_info& info);
    long long analyze_single_function(cgraph_node* node, function_analysis_info& info);
    long long find_triplet_in_type(tree type_decl, vector_triplet& triplet);
    long long analyze_function_operations(tree function_decl, const vector_triplet& triplet, analysis_result& result);
    long long check_pointer_field(tree field, bool& is_pointer);
    long long check_size_field(tree field, bool& is_size_like);
    long long output_results();
    void cleanup_resources();
};

long long VectorAnalyzer::analyze_program() {
    printf("Starting vector container analysis at %s:%d\n", __FILE__, __LINE__);
    
    TRY(analyze_types());
    TRY(analyze_functions());
    TRY(output_results());
    
    printf("Vector analysis completed at %s:%d\n", __FILE__, __LINE__);
    return 0;
}

long long VectorAnalyzer::analyze_types() {
    printf("Analyzing types at %s:%d\n", __FILE__, __LINE__);
    
    // 遍历所有类型定义
    for (tree type_decl = TYPE_MAIN_VARIANT(void_type_node); 
         type_decl; 
         type_decl = TYPE_NEXT_VARIANT(type_decl)) {
        
        if (!TYPE_P(type_decl) || !COMPLETE_TYPE_P(type_decl))
            continue;
            
        if (TREE_CODE(type_decl) != RECORD_TYPE && TREE_CODE(type_decl) != UNION_TYPE)
            continue;
            
        type_analysis_info info;
        long long ret = analyze_single_type(type_decl, info);
        if (ret & ERR_WEAK_ASSERT)
            continue;
        if (ret != 0) {
            printf("Type analysis failed at %s:%d, error=%lld\n", __FILE__, __LINE__, ret);
            return ret;
        }
        
        if (info.is_vector_like) {
            type_results.safe_push(info);
            type_info_map.put(type_decl, &type_results.last());
        }
    }
    
    printf("Found %d vector-like types at %s:%d\n", type_results.length(), __FILE__, __LINE__);
    return 0;
}

long long VectorAnalyzer::analyze_single_type(tree type_decl, type_analysis_info& info) {
    if (!type_decl || !TYPE_P(type_decl))
        return ERR_WEAK_ASSERT;
        
    info.type_decl = type_decl;
    info.is_vector_like = false;
    
    TRY(find_triplet_in_type(type_decl, info.triplet));
    
    // 检查是否找到了完整的三元组
    if (info.triplet.data_ptr && info.triplet.size && info.triplet.capacity) {
        info.is_vector_like = true;
    }
    
    return 0;
}

long long VectorAnalyzer::find_triplet_in_type(tree type_decl, vector_triplet& triplet) {
    if (TREE_CODE(type_decl) != RECORD_TYPE)
        return ERR_WEAK_ASSERT;
        
    tree field = TYPE_FIELDS(type_decl);
    if (!field)
        return ERR_WEAK_ASSERT;
        
    int pointer_count = 0;
    int size_count = 0;
    
    for (; field; field = DECL_CHAIN(field)) {
        if (TREE_CODE(field) != FIELD_DECL)
            continue;
            
        bool is_pointer = false;
        bool is_size_like = false;
        
        TRY_WEAK(check_pointer_field(field, is_pointer));
        TRY_WEAK(check_size_field(field, is_size_like));
        
        if (is_pointer && !triplet.data_ptr) {
            triplet.data_ptr = field;
            pointer_count++;
        } else if (is_size_like) {
            if (!triplet.size) {
                triplet.size = field;
                size_count++;
            } else if (!triplet.capacity) {
                triplet.capacity = field;
                size_count++;
            }
        }
    }
    
    // 检查是否有合理的字段组合
    if (pointer_count >= 1 && size_count >= 2) {
        return 0;
    }
    
    return ERR_WEAK_ASSERT;
}

long long VectorAnalyzer::check_pointer_field(tree field, bool& is_pointer) {
    is_pointer = false;
    
    if (!field || TREE_CODE(field) != FIELD_DECL)
        return ERR_WEAK_ASSERT;
        
    tree field_type = TREE_TYPE(field);
    if (!field_type)
        return ERR_WEAK_ASSERT;
        
    if (TREE_CODE(field_type) == POINTER_TYPE) {
        is_pointer = true;
    }
    
    return 0;
}

long long VectorAnalyzer::check_size_field(tree field, bool& is_size_like) {
    is_size_like = false;
    
    if (!field || TREE_CODE(field) != FIELD_DECL)
        return ERR_WEAK_ASSERT;
        
    tree field_type = TREE_TYPE(field);
    if (!field_type)
        return ERR_WEAK_ASSERT;
        
    // 检查是否是整数类型
    if (INTEGRAL_TYPE_P(field_type)) {
        is_size_like = true;
    }
    
    return 0;
}

long long VectorAnalyzer::analyze_functions() {
    printf("Analyzing functions at %s:%d\n", __FILE__, __LINE__);
    
    cgraph_node *node;
    FOR_EACH_FUNCTION(node) {
        if (!node->definition)
            continue;
            
        function_analysis_info info;
        long long ret = analyze_single_function(node, info);
        if (ret & ERR_WEAK_ASSERT)
            continue;
        if (ret != 0) {
            printf("Function analysis failed at %s:%d, error=%lld\n", __FILE__, __LINE__, ret);
            return ret;
        }
        
        function_results.safe_push(info);
    }
    
    printf("Analyzed %d functions at %s:%d\n", function_results.length(), __FILE__, __LINE__);
    return 0;
}

long long VectorAnalyzer::analyze_single_function(cgraph_node* node, function_analysis_info& info) {
    if (!node || !node->definition)
        return ERR_WEAK_ASSERT;
        
    tree function_decl = node->decl;
    if (!function_decl)
        return ERR_WEAK_ASSERT;
        
    info.function_decl = function_decl;
    info.result = RESULT_IRRELEVANT;
    info.evidence = "No vector operations detected";
    
    // 检查函数是否操作了vector-like类型
    bool found_vector_operation = false;
    
    // 遍历已识别的vector类型，检查函数是否操作这些类型
    for (unsigned i = 0; i < type_results.length(); i++) {
        type_analysis_info& type_info = type_results[i];
        
        analysis_result op_result;
        long long ret = analyze_function_operations(function_decl, type_info.triplet, op_result);
        if (ret & ERR_WEAK_ASSERT)
            continue;
        if (ret != 0)
            return ret;
            
        if (op_result != RESULT_IRRELEVANT) {
            found_vector_operation = true;
            info.result = op_result;
            
            if (op_result == RESULT_SUPPORT) {
                info.evidence = "Function performs vector-like operations";
            } else if (op_result == RESULT_OPPOSE) {
                info.evidence = "Function violates vector semantics";
            }
            break;
        }
    }
    
    return 0;
}

long long VectorAnalyzer::analyze_function_operations(tree function_decl, const vector_triplet& triplet, analysis_result& result) {
    result = RESULT_IRRELEVANT;
    
    if (!function_decl)
        return ERR_WEAK_ASSERT;
        
    function* func = DECL_STRUCT_FUNCTION(function_decl);
    if (!func)
        return ERR_WEAK_ASSERT;
        
    basic_block bb;
    bool has_size_update = false;
    bool has_capacity_check = false;
    bool has_reallocation = false;
    
    FOR_EACH_BB_FN(bb, func) {
        gimple_stmt_iterator gsi;
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple* stmt = gsi_stmt(gsi);
            
            if (gimple_code(stmt) == GIMPLE_ASSIGN) {
                tree lhs = gimple_assign_lhs(stmt);
                tree rhs = gimple_assign_rhs1(stmt);
                
                // 简单的模式匹配：检查是否有size或capacity的更新
                if (lhs && (lhs == triplet.size || lhs == triplet.capacity)) {
                    has_size_update = true;
                }
                
                // 检查是否有容量检查
                if (rhs && (rhs == triplet.capacity || rhs == triplet.size)) {
                    has_capacity_check = true;
                }
            }
            
            if (gimple_code(stmt) == GIMPLE_CALL) {
                tree fndecl = gimple_call_fndecl(stmt);
                if (fndecl) {
                    // 检查是否调用了内存分配函数
                    const char* name = IDENTIFIER_POINTER(DECL_NAME(fndecl));
                    if (name && (strstr(name, "alloc") || strstr(name, "realloc"))) {
                        has_reallocation = true;
                    }
                }
            }
        }
    }
    
    // 基于发现的模式判断结果
    if (has_size_update && has_capacity_check) {
        result = RESULT_SUPPORT;
    } else if (has_reallocation && !has_capacity_check) {
        result = RESULT_OPPOSE;
    } else if (has_size_update || has_capacity_check || has_reallocation) {
        result = RESULT_SUPPORT;
    }
    
    return 0;
}

long long VectorAnalyzer::output_results() {
    printf("\n=== Vector Container Analysis Results ===\n");
    
    printf("\nVector-like Types Found: %d\n", type_results.length());
    for (unsigned i = 0; i < type_results.length(); i++) {
        type_analysis_info& info = type_results[i];
        if (info.type_decl && TYPE_NAME(info.type_decl)) {
            const char* type_name = IDENTIFIER_POINTER(DECL_NAME(TYPE_NAME(info.type_decl)));
            printf("  Type: %s (contains triplet: data_ptr, size, capacity)\n", type_name);
        }
    }
    
    printf("\nFunction Analysis Results: %d\n", function_results.length());
    for (unsigned i = 0; i < function_results.length(); i++) {
        function_analysis_info& info = function_results[i];
        if (info.function_decl && DECL_NAME(info.function_decl)) {
            const char* func_name = IDENTIFIER_POINTER(DECL_NAME(info.function_decl));
            const char* result_str = "";
            
            switch (info.result) {
                case RESULT_SUPPORT:
                    result_str = "SUPPORT";
                    break;
                case RESULT_OPPOSE:
                    result_str = "OPPOSE";
                    break;
                case RESULT_IRRELEVANT:
                    result_str = "IRRELEVANT";
                    break;
            }
            
            if (info.result != RESULT_IRRELEVANT) {
                printf("  Function: %s - %s (%s)\n", func_name, result_str, info.evidence);
            }
        }
    }
    
    printf("\n=== Analysis Complete ===\n");
    return 0;
}

void VectorAnalyzer::cleanup_resources() {
    // cleanup由auto_vec和hash_map自动处理
}

// IPA Pass实现
namespace {
    const pass_data vector_analyzer_pass_data = {
        IPA_PASS,                    // pass类型
        "vector_analyzer",           // pass名称
        OPTGROUP_NONE,              // 优化组
        TV_IPA_FREE_LANG_DATA,      // 时间变量
        0,                          // properties_required
        0,                          // properties_provided
        0,                          // properties_destroyed
        0,                          // todo_flags_start
        0                           // todo_flags_finish
    };

    class vector_analyzer_pass : public ipa_opt_pass_d {
    public:
        vector_analyzer_pass(gcc::context* ctxt)
            : ipa_opt_pass_d(vector_analyzer_pass_data, ctxt,
                           NULL, // generate_summary
                           NULL, // write_summary
                           NULL, // read_summary
                           NULL, // write_optimization_summary
                           NULL, // read_optimization_summary
                           NULL, // stmt_fixup
                           0,    // function_transform_todo_flags_start
                           NULL, // function_transform
                           NULL) // variable_transform
        {}

        virtual unsigned int execute(function*) override {
            VectorAnalyzer analyzer;
            long long ret = analyzer.analyze_program();
            if (ret != 0) {
                printf("Vector analysis failed with error code: %lld\n", ret);
            }
            return 0;
        }
    };
}

// 插件初始化
int plugin_init(struct plugin_name_args* plugin_info,
                struct plugin_gcc_version* version) {
    
    if (!plugin_default_version_check(version, &gcc_version)) {
        printf("Plugin version check failed\n");
        return 1;
    }

    // 注册IPA pass
    struct register_pass_info pass_info;
    pass_info.pass = new vector_analyzer_pass(g);
    pass_info.reference_pass_name = "whole-program";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    
    printf("Vector analyzer plugin initialized\n");
    return 0;
}
