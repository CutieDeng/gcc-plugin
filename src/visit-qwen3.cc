#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <gimple.h>
#include <tree-pass.h>
#include <gimple-iterator.h>
#include <context.h>
#include <tree-pretty-print.h>
#include <attribs.h>

int plugin_is_GPL_compatible;

// 指针访问信息结构
struct ptr_access_info {
    tree ptr_var;
    enum gimple_code access_type;
    location_t loc;
    const char* func_name;
};

// 分析指针表达式
static bool is_pointer_expr(tree expr) {
    if (!expr) return false;
    tree type = TREE_TYPE(expr);
    return type && POINTER_TYPE_P(type);
}

// 获取表达式的字符串表示
static void print_expr_to_buffer(char *buffer, size_t size, tree expr) {
    if (!expr) {
        snprintf(buffer, size, "<null>");
        return;
    }
    
    // 简化处理，实际可以更复杂
    if (DECL_P(expr) && DECL_NAME(expr)) {
        const char *name = IDENTIFIER_POINTER(DECL_NAME(expr));
        snprintf(buffer, size, "%s", name ? name : "<unnamed>");
    } else {
        snprintf(buffer, size, "<expr>");
    }
}

// 分析GIMPLE赋值语句中的指针操作
static void analyze_assign_stmt(gimple *stmt) {
    if (gimple_num_ops(stmt) < 2) return;
    
    tree lhs = gimple_assign_lhs(stmt);
    tree rhs = gimple_assign_rhs1(stmt);
    location_t loc = gimple_location(stmt);
    
    bool lhs_is_ptr = is_pointer_expr(lhs);
    bool rhs_is_ptr = is_pointer_expr(rhs);
    
    if (lhs_is_ptr || rhs_is_ptr) {
        char lhs_buf[128], rhs_buf[128];
        print_expr_to_buffer(lhs_buf, sizeof(lhs_buf), lhs);
        print_expr_to_buffer(rhs_buf, sizeof(rhs_buf), rhs);
        
        fprintf(stderr, "  [指针操作] %s = %s", lhs_buf, rhs_buf);
        
        // 详细分析
        if (lhs_is_ptr && rhs_is_ptr) {
            fprintf(stderr, " (指针到指针赋值)");
        } else if (lhs_is_ptr) {
            fprintf(stderr, " (赋值给指针)");
        } else if (rhs_is_ptr) {
            fprintf(stderr, " (指针值赋值)");
        }
        
        fprintf(stderr, " at line %d\n", LOCATION_LINE(loc));
    }
    
    // 检查结构体成员访问
    if (TREE_CODE(rhs) == COMPONENT_REF) {
        tree object = TREE_OPERAND(rhs, 0);
        tree field = TREE_OPERAND(rhs, 1);
        
        if (is_pointer_expr(object)) {
            fprintf(stderr, "  [结构体指针成员访问] ");
            print_generic_expr(stderr, object, 0);
            fprintf(stderr, "->");
            if (DECL_NAME(field)) {
                fprintf(stderr, "%s", IDENTIFIER_POINTER(DECL_NAME(field)));
            }
            fprintf(stderr, " at line %d\n", LOCATION_LINE(loc));
        }
    }
    
    // 检查数组访问
    if (TREE_CODE(rhs) == ARRAY_REF) {
        tree array = TREE_OPERAND(rhs, 0);
        tree index = TREE_OPERAND(rhs, 1);
        
        if (is_pointer_expr(array)) {
            fprintf(stderr, "  [指针数组访问] ");
            print_generic_expr(stderr, array, 0);
            fprintf(stderr, "[...] at line %d\n", LOCATION_LINE(loc));
        }
    }
}

// 分析函数调用中的指针参数
static void analyze_call_stmt(gimple *stmt) {
    tree fn = gimple_call_fn(stmt);
    location_t loc = gimple_location(stmt);
    int nargs = gimple_call_num_args(stmt);
    
    if (fn) {
        bool has_ptr_args = false;
        for (int i = 0; i < nargs; i++) {
            tree arg = gimple_call_arg(stmt, i);
            if (is_pointer_expr(arg)) {
                has_ptr_args = true;
                break;
            }
        }
        
        if (has_ptr_args) {
            fprintf(stderr, "  [函数调用(含指针参数)] ");
            print_generic_expr(stderr, fn, 0);
            fprintf(stderr, " at line %d\n", LOCATION_LINE(loc));
        }
    }
}

// 分析GIMPLE语句
static void analyze_gimple_stmt(gimple *stmt) {
    enum gimple_code code = gimple_code(stmt);
    
    switch (code) {
        case GIMPLE_ASSIGN:
            analyze_assign_stmt(stmt);
            break;
            
        case GIMPLE_CALL:
            analyze_call_stmt(stmt);
            break;
            
        case GIMPLE_RETURN:
            {
                tree ret = gimple_return_retval(stmt);
                if (ret && is_pointer_expr(ret)) {
                    location_t loc = gimple_location(stmt);
                    fprintf(stderr, "  [指针返回] at line %d\n", LOCATION_LINE(loc));
                }
            }
            break;
            
        default:
            break;
    }
}

// 遍历基本块中的语句
static void analyze_basic_block(basic_block bb) {
    gimple_stmt_iterator gsi;
    for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
        gimple *stmt = gsi_stmt(gsi);
        analyze_gimple_stmt(stmt);
    }
}

// PASS执行函数
static unsigned int execute_ptr_access_pass(void) {
    if (!cfun) return 0;
    
    const char *func_name = "unknown";
    if (DECL_NAME(current_function_decl)) {
        func_name = IDENTIFIER_POINTER(DECL_NAME(current_function_decl));
    }
    fprintf(stderr, "\n=== 分析函数: %s ===\n", func_name);
    
    // 遍历所有基本块
    basic_block bb;
    FOR_EACH_BB_FN(bb, cfun) {
        analyze_basic_block(bb);
    }
    
    fprintf(stderr, "=== 函数分析完成 ===\n\n");
    return 0;
}

// PASS定义
namespace {
const pass_data ptr_access_pass_data = {
    GIMPLE_PASS,           // type
    "ptr_access",          // name
    OPTGROUP_NONE,         // optinfo_flags
    TV_NONE,               // tv_id
    0,                     // properties_required
    0,                     // properties_provided
    0,                     // properties_destroyed
    0,                     // todo_flags_start
    0,                     // todo_flags_finish
};

class pass_ptr_access : public gimple_opt_pass {
public:
    pass_ptr_access(gcc::context *ctxt)
        : gimple_opt_pass(ptr_access_pass_data, ctxt)
    {}

    unsigned int execute(function *fun) {
        return execute_ptr_access_pass();
    }
};
} // namespace

// 插件初始化函数
int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        return 1;
    }

    struct register_pass_info gimple_pass_info;
    gimple_pass_info.pass = new pass_ptr_access(NULL);
    gimple_pass_info.reference_pass_name = "ssa";
    gimple_pass_info.ref_pass_instance_number = 1;
    gimple_pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &gimple_pass_info);
    
    fprintf(stderr, "GCC指针访问分析插件已加载\n");
    fprintf(stderr, "支持功能：指针赋值分析、结构体成员访问、数组访问、函数调用分析\n");
    
    return 0;
}
