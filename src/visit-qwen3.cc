#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <gimple.h>
#include <tree-pass.h>
#include <gimple-iterator.h>
#include <gimple-ssa.h>
#include <ipa-pass.h>
#include <ipa-prop.h>
#include <vec.h>
#include <hash-map.h>
#include <hash-table.h>
#include <basic-block.h>
#include <function.h>
#include <tree-ssa-alias.h>
#include <gimple-expr.h>
#include <tree-pretty-print.h>
#include <print-tree.h>
#include <stringpool.h>
#include <cgraph.h>

int plugin_is_GPL_compatible;

// 访问类型枚举
enum access_type {
    ACCESS_READ,
    ACCESS_WRITE
};

// 访问记录结构
struct access_info {
    tree struct_type;           // 结构体类型
    tree field;                 // 字段
    tree element_type;          // 指针所指向元素的类型
    gimple *member_access_stmt; // 访问成员的gimple语句
    gimple *element_access_stmt;// 访问元素的gimple语句
    enum access_type access_kind; // 访问类型
};

// 向量存储所有访问信息
static vec<access_info *> access_records;

// 获取访问类型
static enum access_type get_access_type(gimple *stmt) {
    if (gimple_assign_single_p(stmt)) {
        tree lhs = gimple_assign_lhs(stmt);
        if (lhs && TREE_CODE(lhs) == INDIRECT_REF) {
            return ACCESS_WRITE;
        }
    } else if (gimple_assign_copy_p(stmt)) {
        tree rhs = gimple_assign_rhs1(stmt);
        if (rhs && TREE_CODE(rhs) == INDIRECT_REF) {
            return ACCESS_READ;
        }
    } else if (gimple_call_p(stmt)) {
        // 处理函数调用中的参数传递
        for (unsigned i = 0; i < gimple_call_num_args(stmt); i++) {
            tree arg = gimple_call_arg(stmt, i);
            if (arg && TREE_CODE(arg) == INDIRECT_REF) {
                return ACCESS_READ;
            }
        }
    }
    return ACCESS_READ; // 默认为读取
}

// 检查是否为结构体指针的成员访问
static bool is_struct_member_access(gimple *stmt, tree *struct_type, tree *field, tree *accessed_var) {
    if (!stmt || !gimple_assign_single_p(stmt))
        return false;

    tree lhs = gimple_assign_lhs(stmt);
    tree rhs = gimple_assign_rhs1(stmt);

    // 检查形如: temp = struct_ptr->field 或 temp = (*struct_ptr).field
    if (lhs && TREE_CODE(lhs) == SSA_NAME) {
        if (rhs && TREE_CODE(rhs) == COMPONENT_REF) {
            tree object = TREE_OPERAND(rhs, 0);
            tree field_decl = TREE_OPERAND(rhs, 1);
            
            // 检查是否是指针解引用后的结构体访问
            if (TREE_CODE(object) == INDIRECT_REF || TREE_CODE(object) == MEM_REF) {
                tree ptr_type = TREE_TYPE(object);
                if (ptr_type && TREE_CODE(ptr_type) == POINTER_TYPE) {
                    tree pointed_type = TREE_TYPE(ptr_type);
                    if (pointed_type && TREE_CODE(pointed_type) == RECORD_TYPE) {
                        *struct_type = pointed_type;
                        *field = field_decl;
                        *accessed_var = lhs;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// 分析指针访问的后续使用
static void analyze_pointer_usage(gimple_stmt_iterator gsi, gimple *member_stmt, 
                                  tree struct_type, tree field, tree ptr_var) {
    if (!ptr_var || TREE_CODE(ptr_var) != SSA_NAME)
        return;

    // 获取指针的基本类型
    tree ptr_type = TREE_TYPE(ptr_var);
    if (!ptr_type || TREE_CODE(ptr_type) != POINTER_TYPE)
        return;

    tree element_type = TREE_TYPE(ptr_type);

    // 遍历后续语句寻找对该指针的使用
    gimple_stmt_iterator next_gsi = gsi;
    while (!gsi_end_p(next_gsi)) {
        gimple *stmt = gsi_stmt(next_gsi);
        if (!stmt) {
            gsi_next(&next_gsi);
            continue;
        }

        bool found_access = false;
        
        // 检查赋值语句的左右操作数
        if (gimple_assign_single_p(stmt)) {
            tree lhs = gimple_assign_lhs(stmt);
            tree rhs = gimple_assign_rhs1(stmt);
            
            // 检查写入操作: *ptr = value
            if (lhs && TREE_CODE(lhs) == INDIRECT_REF) {
                tree ref_ptr = TREE_OPERAND(lhs, 0);
                if (ref_ptr == ptr_var || (TREE_CODE(ref_ptr) == SSA_NAME && 
                    TREE_CODE(ptr_var) == SSA_NAME && 
                    gimple_get_def_for_ssa_name(ref_ptr) == gimple_get_def_for_ssa_name(ptr_var))) {
                    access_info *info = new access_info();
                    info->struct_type = struct_type;
                    info->field = field;
                    info->element_type = element_type;
                    info->member_access_stmt = member_stmt;
                    info->element_access_stmt = stmt;
                    info->access_kind = ACCESS_WRITE;
                    access_records.safe_push(info);
                    found_access = true;
                }
            }
            
            // 检查读取操作: value = *ptr
            if (rhs && TREE_CODE(rhs) == INDIRECT_REF) {
                tree ref_ptr = TREE_OPERAND(rhs, 0);
                if (ref_ptr == ptr_var || (TREE_CODE(ref_ptr) == SSA_NAME && 
                    TREE_CODE(ptr_var) == SSA_NAME && 
                    gimple_get_def_for_ssa_name(ref_ptr) == gimple_get_def_for_ssa_name(ptr_var))) {
                    access_info *info = new access_info();
                    info->struct_type = struct_type;
                    info->field = field;
                    info->element_type = element_type;
                    info->member_access_stmt = member_stmt;
                    info->element_access_stmt = stmt;
                    info->access_kind = ACCESS_READ;
                    access_records.safe_push(info);
                    found_access = true;
                }
            }
        }
        
        // 检查函数调用中的参数
        if (gimple_call_p(stmt)) {
            for (unsigned i = 0; i < gimple_call_num_args(stmt); i++) {
                tree arg = gimple_call_arg(stmt, i);
                if (arg && TREE_CODE(arg) == INDIRECT_REF) {
                    tree ref_ptr = TREE_OPERAND(arg, 0);
                    if (ref_ptr == ptr_var || (TREE_CODE(ref_ptr) == SSA_NAME && 
                        TREE_CODE(ptr_var) == SSA_NAME && 
                        gimple_get_def_for_ssa_name(ref_ptr) == gimple_get_def_for_ssa_name(ptr_var))) {
                        access_info *info = new access_info();
                        info->struct_type = struct_type;
                        info->field = field;
                        info->element_type = element_type;
                        info->member_access_stmt = member_stmt;
                        info->element_access_stmt = stmt;
                        info->access_kind = ACCESS_READ;
                        access_records.safe_push(info);
                        found_access = true;
                    }
                }
            }
        }

        if (found_access) {
            break; // 找到一次访问就停止
        }
        
        gsi_next(&next_gsi);
    }
}

// 分析函数中的二级指针访问
static void analyze_function_access(function *fn) {
    if (!fn || !fn->gimple_body)
        return;

    basic_block bb;
    FOR_EACH_BB_FN(bb, fn) {
        gimple_stmt_iterator gsi;
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            
            tree struct_type = NULL_TREE;
            tree field = NULL_TREE;
            tree accessed_var = NULL_TREE;
            
            // 检查是否为结构体成员访问
            if (is_struct_member_access(stmt, &struct_type, &field, &accessed_var)) {
                // 分析该成员变量的后续使用
                analyze_pointer_usage(gsi, stmt, struct_type, field, accessed_var);
            }
        }
    }
}

// 打印访问记录
static void print_access_records(void) {
    fprintf(stderr, "=== 二级指针访问分析结果 ===\n");
    fprintf(stderr, "共发现 %d 条访问记录\n", access_records.length());
    
    for (unsigned i = 0; i < access_records.length(); i++) {
        access_info *info = access_records[i];
        
        fprintf(stderr, "\n记录 %u:\n", i + 1);
        fprintf(stderr, "  结构体类型: %s\n", 
                get_tree_code_name(TREE_CODE(info->struct_type)));
        
        if (DECL_NAME(info->field)) {
            fprintf(stderr, "  字段: %s\n", 
                    IDENTIFIER_POINTER(DECL_NAME(info->field)));
        } else {
            fprintf(stderr, "  字段: <unnamed>\n");
        }
        
        fprintf(stderr, "  元素类型: %s\n", 
                get_tree_code_name(TREE_CODE(info->element_type)));
        fprintf(stderr, "  访问类型: %s\n", 
                info->access_kind == ACCESS_WRITE ? "写入" : "读取");
        
        fprintf(stderr, "  成员访问语句: ");
        print_gimple_stmt(stderr, info->member_access_stmt, 0, TDF_VOPS);
        
        fprintf(stderr, "  元素访问语句: ");
        print_gimple_stmt(stderr, info->element_access_stmt, 0, TDF_VOPS);
    }
    fprintf(stderr, "\n");
}

// IPA PASS回调函数
static unsigned int execute_ipa_pass(void) {
    struct cgraph_node *node;
    
    // 清空之前的记录
    access_records.truncate(0);
    
    // 遍历所有函数
    FOR_EACH_FUNCTION(node) {
        function *fn = node->get_fun();
        if (fn && fn->gimple_body) {
            analyze_function_access(fn);
        }
    }
    
    // 打印结果
    print_access_records();
    
    return 0;
}

// IPA PASS定义
namespace {
const struct pass_data ipa_ptr_access_pass_data = {
    IPA_PASS,                         // type
    "ptr_access",                     // name
    OPTGROUP_NONE,                    // optinfo_flags
    TV_IPA_PTR_ACCESS,                // tv_id
    0,                                // properties_required
    0,                                // properties_provided
    0,                                // properties_destroyed
    0,                                // todo_flags_start
    ( TODO_dump_func | TODO_verify_ssa | 
      TODO_verify_stmts | TODO_update_ssa ), // todo_flags_finish
};

class ipa_ptr_access_pass : public ipa_opt_pass_d {
public:
    ipa_ptr_access_pass() : ipa_opt_pass_d(ipa_ptr_access_pass_data, NULL, NULL, NULL,
                                           NULL, NULL, NULL, execute_ipa_pass, NULL,
                                           NULL, NULL) {}
};

}

// 插件初始化函数
__visible int plugin_init(struct plugin_name_args *plugin_info,
                         struct plugin_gcc_version *version) {
    // 版本检查
    if (!plugin_default_version_check(version, &gcc_version)) {
        error("插件版本不兼容");
        return 1;
    }

    // 注册IPA PASS
    struct register_pass_info pass_info;
    pass_info.pass = new ipa_ptr_access_pass();
    pass_info.reference_pass_name = "pta";  // 在指针分析之后执行
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    
    // 注册清理回调
    register_callback(plugin_info->base_name, PLUGIN_FINISH, NULL, NULL);
    
    fprintf(stderr, "GCC指针访问分析插件已加载\n");
    
    return 0;
}

// 插件信息
__visible struct plugin_gcc_version plugin_version = {
    GCCPLUGIN_VERSION_MAJOR, GCCPLUGIN_VERSION_MINOR
};
