#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <gimple.h>
#include <tree-pass.h>
#include <gimple-iterator.h>
#include <context.h>

int plugin_is_GPL_compatible;

// 分析GIMPLE语句
static void analyze_gimple_stmt(gimple *stmt) {
    enum gimple_code code = gimple_code(stmt);
    
    switch (code) {
        case GIMPLE_ASSIGN:
            // 处理赋值语句
            if (gimple_num_ops(stmt) >= 2) {
                tree lhs = gimple_assign_lhs(stmt);
                tree rhs = gimple_assign_rhs1(stmt);
                
                // 检查是否涉及指针
                if (POINTER_TYPE_P(TREE_TYPE(lhs)) || POINTER_TYPE_P(TREE_TYPE(rhs))) {
                    fprintf(stderr, "  发现指针赋值: ");
                    print_generic_expr(stderr, lhs, 0);
                    fprintf(stderr, " = ");
                    print_generic_expr(stderr, rhs, 0);
                    fprintf(stderr, "\n");
                }
            }
            break;
            
        case GIMPLE_CALL:
            // 处理函数调用
            {
                tree fn = gimple_call_fn(stmt);
                if (fn) {
                    fprintf(stderr, "  函数调用: ");
                    print_generic_expr(stderr, fn, 0);
                    fprintf(stderr, "\n");
                }
            }
            break;
            
        default:
            break;
    }
}

// 遍历基本块中的语句
static void analyze_basic_block_basic(basic_block bb) {
    gimple_stmt_iterator gsi;
    for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
        gimple *stmt = gsi_stmt(gsi);
        analyze_gimple_stmt(stmt);
    }
}

// PASS执行函数
static unsigned int execute_ptr_access_pass(void) {
    if (!cfun) return 0;
    
    fprintf(stderr, "分析函数: %s\n", 
            (DECL_NAME(current_function_decl)) ? 
            IDENTIFIER_POINTER(DECL_NAME(current_function_decl)) : "unknown");
    
    // 遍历所有基本块
    basic_block bb;
    FOR_EACH_BB_FN(bb, cfun) {
        analyze_basic_block_basic(bb);
    }
    
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
    
    return 0;
}
