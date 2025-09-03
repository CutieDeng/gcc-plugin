#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <tree-cfg.h>
#include <basic-block.h>
#include <function.h>
#include <vec.h>
#include <hash-map.h>
#include <stdio.h>
#include <stdlib.h>

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
#define PATTERN_SAFE_CHECK_WEAK() do { if (ret == ErrorStrongAssert) { return ret; } } while (0)
#define PATTERN_MATCH(x, y, e) do { \
    if ((x) == 0) { \
        (y) = (x); \
    } else if ((y) != (x)) { \
        ret = (e); \
    } \
} while (0)
#define PATTERN_END return ret;
#define DEBUG(fmt, ...) printf("[%s:%d %s] " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)

int plugin_is_GPL_compatible;

struct VectorCandidate {
    tree type_decl;
    tree ptr_field;
    tree size_field;
    tree capacity_field;
    char const *type_name;
};

enum EvidenceType {
    EVIDENCE_SUPPORT,
    EVIDENCE_OPPOSE, 
    EVIDENCE_NEUTRAL
};

struct OperationEvidence {
    char const *operation_desc;
    EvidenceType evidence_type;
    char const *reason;
};

struct FunctionAnalysis {
    tree function_decl;
    char const *function_name;
    vec<OperationEvidence> evidences;
};

class VectorAnalysis {
private:
    vec<VectorCandidate> candidates;
    vec<FunctionAnalysis> function_analyses;
    hash_map<tree, VectorCandidate*> type_to_candidate;
    
public:
    err_t init();
    err_t scan_types();
    err_t analyze_function(function *fn);
    err_t analyze_gimple_stmt(gimple *stmt, FunctionAnalysis &analysis);
    err_t check_vector_operation(tree lhs, tree rhs, FunctionAnalysis &analysis);
    err_t is_vector_candidate_type(tree type, VectorCandidate **candidate);
    err_t collect_results();
    
private:
    err_t find_triplet_fields(tree type, tree *ptr_field, tree *size_field, tree *capacity_field);
    err_t is_pointer_field(tree field);
    err_t is_size_field(tree field);
    char const *get_type_name(tree type);
    char const *get_function_name(tree fn_decl);
};

VectorAnalysis *global_analysis = nullptr;

err_t VectorAnalysis::init() {
    PATTERN_BEGIN
    
    candidates.create(16);
    function_analyses.create(64);
    type_to_candidate.create(32);
    
    DEBUG("VectorAnalysis initialized");
    
    PATTERN_END
}

err_t VectorAnalysis::scan_types() {
    PATTERN_BEGIN
    
    // 遍历所有类型定义
    for (tree type = TYPE_STUB_DECL(void_type_node); type; type = TREE_CHAIN(type)) {
        if (!type || TREE_CODE(type) != TYPE_DECL) continue;
        
        tree type_node = TREE_TYPE(type);
        if (!type_node || (TREE_CODE(type_node) != RECORD_TYPE && TREE_CODE(type_node) != UNION_TYPE)) {
            continue;
        }
        
        tree ptr_field = nullptr;
        tree size_field = nullptr; 
        tree capacity_field = nullptr;
        
        err_t find_ret = find_triplet_fields(type_node, &ptr_field, &size_field, &capacity_field);
        if (find_ret == ErrorOk && ptr_field && size_field && capacity_field) {
            VectorCandidate candidate;
            candidate.type_decl = type;
            candidate.ptr_field = ptr_field;
            candidate.size_field = size_field;
            candidate.capacity_field = capacity_field;
            candidate.type_name = get_type_name(type_node);
            
            candidates.safe_push(candidate);
            type_to_candidate.put(type_node, &candidates.last());
            
            DEBUG("Found vector candidate: %s", candidate.type_name ? candidate.type_name : "unknown");
        }
    }
    
    DEBUG("Scanned %d vector candidates", candidates.length());
    
    PATTERN_END
}

err_t VectorAnalysis::find_triplet_fields(tree type, tree *ptr_field, tree *size_field, tree *capacity_field) {
    PATTERN_BEGIN
    
    *ptr_field = nullptr;
    *size_field = nullptr;
    *capacity_field = nullptr;
    
    int pointer_count = 0;
    int size_count = 0;
    
    for (tree field = TYPE_FIELDS(type); field; field = DECL_CHAIN(field)) {
        if (TREE_CODE(field) != FIELD_DECL) continue;
        
        tree field_type = TREE_TYPE(field);
        if (!field_type) continue;
        
        // 检查是否为指针字段
        if (TREE_CODE(field_type) == POINTER_TYPE) {
            if (!*ptr_field) {
                *ptr_field = field;
                pointer_count++;
            }
        }
        // 检查是否为大小相关字段
        else if (TREE_CODE(field_type) == INTEGER_TYPE) {
            if (!*size_field) {
                *size_field = field;
                size_count++;
            } else if (!*capacity_field) {
                *capacity_field = field;
                size_count++;
            }
        }
    }
    
    // 需要至少一个指针字段和两个大小字段
    if (pointer_count >= 1 && size_count >= 2) {
        ret = ErrorOk;
    } else {
        ret = ErrorWeakAssert;
    }
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_function(function *fn) {
    PATTERN_BEGIN
    
    if (!fn || !fn->decl) {
        ret = ErrorGccLogic;
        PATTERN_SAFE_CHECK_STRONG();
    }
    
    FunctionAnalysis analysis;
    analysis.function_decl = fn->decl;
    analysis.function_name = get_function_name(fn->decl);
    analysis.evidences.create(8);
    
    DEBUG("Analyzing function: %s", analysis.function_name ? analysis.function_name : "unknown");
    
    basic_block bb;
    FOR_EACH_BB_FN(bb, fn) {
        gimple_stmt_iterator gsi;
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            if (stmt) {
                err_t analyze_ret = analyze_gimple_stmt(stmt, analysis);
                PATTERN_SAFE_CHECK_WEAK();
            }
        }
    }
    
    function_analyses.safe_push(analysis);
    
    DEBUG("Function %s analysis complete with %d evidences", 
          analysis.function_name ? analysis.function_name : "unknown",
          analysis.evidences.length());
    
    PATTERN_END
}

err_t VectorAnalysis::analyze_gimple_stmt(gimple *stmt, FunctionAnalysis &analysis) {
    PATTERN_BEGIN
    
    if (!stmt) {
        ret = ErrorGccLogic;
        PATTERN_SAFE_CHECK_STRONG();
    }
    
    // 分析赋值语句
    if (gimple_code(stmt) == GIMPLE_ASSIGN) {
        tree lhs = gimple_assign_lhs(stmt);
        tree rhs = gimple_assign_rhs1(stmt);
        
        if (lhs && rhs) {
            err_t check_ret = check_vector_operation(lhs, rhs, analysis);
            PATTERN_SAFE_CHECK_WEAK();
        }
    }
    // 分析函数调用
    else if (gimple_code(stmt) == GIMPLE_CALL) {
        tree fn_decl = gimple_call_fndecl(stmt);
        if (fn_decl) {
            char const *fn_name = get_function_name(fn_decl);
            
            // 检查是否为内存相关函数调用
            if (fn_name && (strstr(fn_name, "malloc") || strstr(fn_name, "realloc") || strstr(fn_name, "free"))) {
                OperationEvidence evidence;
                evidence.operation_desc = "memory_management_call";
                evidence.evidence_type = EVIDENCE_SUPPORT;
                evidence.reason = "Memory management suggests dynamic array";
                analysis.evidences.safe_push(evidence);
            }
        }
    }
    
    PATTERN_END
}

err_t VectorAnalysis::check_vector_operation(tree lhs, tree rhs, FunctionAnalysis &analysis) {
    PATTERN_BEGIN
    
    if (!lhs || !rhs) {
        ret = ErrorGccLogic;
        PATTERN_SAFE_CHECK_STRONG();
    }
    
    // 检查左值是否为vector候选类型的字段访问
    if (TREE_CODE(lhs) == COMPONENT_REF) {
        tree obj = TREE_OPERAND(lhs, 0);
        tree field = TREE_OPERAND(lhs, 1);
        
        if (obj && field && TREE_TYPE(obj)) {
            VectorCandidate *candidate = nullptr;
            err_t is_candidate_ret = is_vector_candidate_type(TREE_TYPE(obj), &candidate);
            
            if (is_candidate_ret == ErrorOk && candidate) {
                OperationEvidence evidence;
                evidence.operation_desc = "field_assignment";
                
                // 检查是否为关键字段的操作
                if (field == candidate->ptr_field) {
                    evidence.evidence_type = EVIDENCE_SUPPORT;
                    evidence.reason = "Pointer field assignment suggests array management";
                } else if (field == candidate->size_field) {
                    evidence.evidence_type = EVIDENCE_SUPPORT;
                    evidence.reason = "Size field assignment suggests size tracking";
                } else if (field == candidate->capacity_field) {
                    evidence.evidence_type = EVIDENCE_SUPPORT;
                    evidence.reason = "Capacity field assignment suggests capacity management";
                } else {
                    evidence.evidence_type = EVIDENCE_NEUTRAL;
                    evidence.reason = "Non-vector field assignment";
                }
                
                analysis.evidences.safe_push(evidence);
            }
        }
    }
    
    PATTERN_END
}

err_t VectorAnalysis::is_vector_candidate_type(tree type, VectorCandidate **candidate) {
    PATTERN_BEGIN
    
    *candidate = nullptr;
    
    if (!type) {
        ret = ErrorGccLogic;
        PATTERN_SAFE_CHECK_STRONG();
    }
    
    VectorCandidate **found = type_to_candidate.get(type);
    if (found) {
        *candidate = *found;
        ret = ErrorOk;
    } else {
        ret = ErrorWeakAssert;
    }
    
    PATTERN_END
}

err_t VectorAnalysis::collect_results() {
    PATTERN_BEGIN
    
    printf("\n=== Vector Container Analysis Results ===\n");
    printf("Found %d vector candidate types:\n", candidates.length());
    
    for (unsigned i = 0; i < candidates.length(); i++) {
        VectorCandidate &candidate = candidates[i];
        printf("  Type: %s\n", candidate.type_name ? candidate.type_name : "unknown");
        printf("    Pointer field: %s\n", 
               DECL_NAME(candidate.ptr_field) ? IDENTIFIER_POINTER(DECL_NAME(candidate.ptr_field)) : "unnamed");
        printf("    Size field: %s\n",
               DECL_NAME(candidate.size_field) ? IDENTIFIER_POINTER(DECL_NAME(candidate.size_field)) : "unnamed");
        printf("    Capacity field: %s\n",
               DECL_NAME(candidate.capacity_field) ? IDENTIFIER_POINTER(DECL_NAME(candidate.capacity_field)) : "unnamed");
    }
    
    printf("\nFunction Analysis Results (%d functions):\n", function_analyses.length());
    
    for (unsigned i = 0; i < function_analyses.length(); i++) {
        FunctionAnalysis &analysis = function_analyses[i];
        printf("  Function: %s\n", analysis.function_name ? analysis.function_name : "unknown");
        
        int support_count = 0;
        int oppose_count = 0;
        int neutral_count = 0;
        
        for (unsigned j = 0; j < analysis.evidences.length(); j++) {
            OperationEvidence &evidence = analysis.evidences[j];
            switch (evidence.evidence_type) {
                case EVIDENCE_SUPPORT: support_count++; break;
                case EVIDENCE_OPPOSE: oppose_count++; break;
                case EVIDENCE_NEUTRAL: neutral_count++; break;
            }
            printf("    [%s] %s: %s\n",
                   evidence.evidence_type == EVIDENCE_SUPPORT ? "SUPPORT" :
                   evidence.evidence_type == EVIDENCE_OPPOSE ? "OPPOSE" : "NEUTRAL",
                   evidence.operation_desc,
                   evidence.reason);
        }
        
        printf("    Summary: %d support, %d oppose, %d neutral\n", 
               support_count, oppose_count, neutral_count);
        
        // 给出总体判断
        if (support_count > oppose_count && support_count > 0) {
            printf("    Conclusion: VECTOR-LIKE BEHAVIOR\n");
        } else if (oppose_count > support_count) {
            printf("    Conclusion: NON-VECTOR BEHAVIOR\n");
        } else {
            printf("    Conclusion: INSUFFICIENT EVIDENCE\n");
        }
    }
    
    printf("=== End of Analysis ===\n");
    
    PATTERN_END
}

char const *VectorAnalysis::get_type_name(tree type) {
    if (!type) return nullptr;
    
    tree name = TYPE_NAME(type);
    if (name) {
        if (TREE_CODE(name) == TYPE_DECL && DECL_NAME(name)) {
            return IDENTIFIER_POINTER(DECL_NAME(name));
        } else if (TREE_CODE(name) == IDENTIFIER_NODE) {
            return IDENTIFIER_POINTER(name);
        }
    }
    return nullptr;
}

char const *VectorAnalysis::get_function_name(tree fn_decl) {
    if (!fn_decl || !DECL_NAME(fn_decl)) return nullptr;
    return IDENTIFIER_POINTER(DECL_NAME(fn_decl));
}

// Pass implementation
namespace {
    const pass_data vector_analysis_pass_data = {
        GIMPLE_PASS,
        "vector_analysis",
        OPTGROUP_NONE,
        TV_NONE,
        PROP_gimple_any,
        0,
        0,
        0,
        0
    };
    
    class vector_analysis_pass : public gimple_opt_pass {
    public:
        vector_analysis_pass(gcc::context *ctxt)
            : gimple_opt_pass(vector_analysis_pass_data, ctxt) {
            ref_pass_instance_number = 1;
        }
        
        virtual unsigned int execute(function *fn) override {
            if (!global_analysis) {
                global_analysis = new VectorAnalysis();
                global_analysis->init();
                global_analysis->scan_types();
            }
            
            if (fn) {
                global_analysis->analyze_function(fn);
            }
            
            return 0;
        }
        
        virtual vector_analysis_pass *clone() override {
            return this;
        }
    };
}

static void finish_analysis(void *gcc_data, void *user_data) {
    if (global_analysis) {
        global_analysis->collect_results();
        delete global_analysis;
        global_analysis = nullptr;
    }
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        return 1;
    }
    
    struct register_pass_info pass_info;
    pass_info.pass = new vector_analysis_pass(g);
    pass_info.reference_pass_name = "cfg";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, nullptr, &pass_info);
    register_callback(plugin_info->base_name, PLUGIN_FINISH, finish_analysis, nullptr);
    
    return 0;
}
