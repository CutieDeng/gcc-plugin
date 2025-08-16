#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <gimple-walk.h>
#include <cgraph.h>
#include <diagnostic.h>
#include <print-tree.h>
#include <tree-cfg.h>
#include <stringpool.h>
#include <attribs.h>
#include <vec.h>
#include <hash-map.h>
#include <hash-set.h>
#include <gimple-pretty-print.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>

int plugin_is_GPL_compatible;

// 访问类型枚举
enum access_type {
    ACCESS_READ = 1,
    ACCESS_WRITE = 2,
    ACCESS_RW = 3
};

// 访问信息结构体
struct access_info {
    tree struct_type;           // 结构体类型
    tree field_decl;            // 字段声明
    tree element_type;          // 元素类型
    gimple *member_stmt;        // 访问成员的gimple语句
    gimple *element_stmt;       // 访问元素的gimple语句
    enum access_type acc_type;  // 访问类型
    location_t loc;             // 源码位置
    std::string func_name;      // 函数名
    
    access_info() : struct_type(NULL_TREE), field_decl(NULL_TREE), 
                   element_type(NULL_TREE), member_stmt(NULL), 
                   element_stmt(NULL), acc_type(ACCESS_READ), 
                   loc(UNKNOWN_LOCATION), func_name("") {}
};

// 全局收集器
static std::vector<access_info> collected_accesses;
static const char *plugin_name = "double_pointer_access";

// 辅助函数：获取类型名称
static std::string get_type_name(tree type) {
    if (!type) return "<unknown>";
    
    if (TYPE_NAME(type)) {
        if (TREE_CODE(TYPE_NAME(type)) == IDENTIFIER_NODE) {
            return IDENTIFIER_POINTER(TYPE_NAME(type));
        } else if (TREE_CODE(TYPE_NAME(type)) == TYPE_DECL) {
            tree decl_name = DECL_NAME(TYPE_NAME(type));
            if (decl_name)
                return IDENTIFIER_POINTER(decl_name);
        }
    }
    
    // 基本类型处理
    switch (TREE_CODE(type)) {
        case INTEGER_TYPE:
            if (TYPE_PRECISION(type) == 8)
                return TYPE_UNSIGNED(type) ? "unsigned char" : "char";
            else if (TYPE_PRECISION(type) == 16)
                return TYPE_UNSIGNED(type) ? "unsigned short" : "short";
            else if (TYPE_PRECISION(type) == 32)
                return TYPE_UNSIGNED(type) ? "unsigned int" : "int";
            else if (TYPE_PRECISION(type) == 64)
                return TYPE_UNSIGNED(type) ? "unsigned long" : "long";
            return "integer";
        case REAL_TYPE:
            if (TYPE_PRECISION(type) == 32)
                return "float";
            else if (TYPE_PRECISION(type) == 64)
                return "double";
            return "real";
        case POINTER_TYPE:
            return "pointer to " + get_type_name(TREE_TYPE(type));
        case ARRAY_TYPE:
            return "array of " + get_type_name(TREE_TYPE(type));
        case RECORD_TYPE:
            return "struct";
        case UNION_TYPE:
            return "union";
        default:
            return "<complex type>";
    }
}

// 辅助函数：获取字段名称
static std::string get_field_name(tree field) {
    if (!field || TREE_CODE(field) != FIELD_DECL)
        return "<unknown>";
    
    tree field_name = DECL_NAME(field);
    if (field_name)
        return IDENTIFIER_POINTER(field_name);
    return "<anonymous>";
}

// 检查是否是二级指针访问
static bool is_double_pointer_access(tree expr, access_info &info) {
    if (!expr) return false;
    
    // 处理 COMPONENT_REF: struct_ptr->field
    if (TREE_CODE(expr) == COMPONENT_REF) {
        tree base = TREE_OPERAND(expr, 0);
        tree field = TREE_OPERAND(expr, 1);
        
        // 检查基址是否是间接引用
        if (TREE_CODE(base) == INDIRECT_REF || TREE_CODE(base) == MEM_REF) {
            tree ptr_expr = (TREE_CODE(base) == INDIRECT_REF) ? 
                            TREE_OPERAND(base, 0) : TREE_OPERAND(base, 0);
            tree ptr_type = TREE_TYPE(ptr_expr);
            
            // 确保是指向结构体的指针
            if (TREE_CODE(ptr_type) == POINTER_TYPE) {
                tree struct_type = TREE_TYPE(ptr_type);
                if (TREE_CODE(struct_type) == RECORD_TYPE || 
                    TREE_CODE(struct_type) == UNION_TYPE) {
                    // 检查字段是否是指针类型
                    tree field_type = TREE_TYPE(field);
                    if (TREE_CODE(field_type) == POINTER_TYPE) {
                        info.struct_type = struct_type;
                        info.field_decl = field;
                        info.element_type = TREE_TYPE(field_type);
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

// 递归查找SSA定义链
static gimple* find_pointer_source(tree ptr, access_info &info) {
    if (TREE_CODE(ptr) != SSA_NAME)
        return NULL;
    
    gimple *def_stmt = SSA_NAME_DEF_STMT(ptr);
    if (!def_stmt)
        return NULL;
    
    if (gimple_code(def_stmt) == GIMPLE_ASSIGN) {
        tree rhs = gimple_assign_rhs1(def_stmt);
        if (is_double_pointer_access(rhs, info)) {
            return def_stmt;
        }
        
        // 继续递归查找
        if (TREE_CODE(rhs) == SSA_NAME) {
            return find_pointer_source(rhs, info);
        }
    } else if (gimple_code(def_stmt) == GIMPLE_PHI) {
        // 处理PHI节点，检查所有来源
        gphi *phi = as_a<gphi*>(def_stmt);
        for (unsigned i = 0; i < gimple_phi_num_args(phi); i++) {
            tree arg = gimple_phi_arg_def(phi, i);
            gimple *source = find_pointer_source(arg, info);
            if (source)
                return source;
        }
    }
    
    return NULL;
}

// 分析单个gimple语句
static void analyze_gimple_stmt(gimple *stmt, function *fun) {
    if (!stmt) return;
    
    access_info info;
    info.loc = gimple_location(stmt);
    info.func_name = function_name(fun);
    
    enum gimple_code code = gimple_code(stmt);
    
    // 处理赋值语句
    if (code == GIMPLE_ASSIGN) {
        tree lhs = gimple_assign_lhs(stmt);
        tree rhs1 = gimple_assign_rhs1(stmt);
        
        // 检查左值（写操作）
        if (TREE_CODE(lhs) == MEM_REF || TREE_CODE(lhs) == INDIRECT_REF) {
            tree ptr = TREE_OPERAND(lhs, 0);
            gimple *def_stmt = find_pointer_source(ptr, info);
            if (def_stmt) {
                info.member_stmt = def_stmt;
                info.element_stmt = stmt;
                info.acc_type = ACCESS_WRITE;
                collected_accesses.push_back(info);
            }
        }
        
        // 检查右值（读操作）
        if (TREE_CODE(rhs1) == MEM_REF || TREE_CODE(rhs1) == INDIRECT_REF) {
            tree ptr = TREE_OPERAND(rhs1, 0);
            gimple *def_stmt = find_pointer_source(ptr, info);
            if (def_stmt) {
                info.member_stmt = def_stmt;
                info.element_stmt = stmt;
                info.acc_type = ACCESS_READ;
                collected_accesses.push_back(info);
            }
        }
        
        // 处理数组访问形式 (*p)[i]
        if (TREE_CODE(lhs) == ARRAY_REF) {
            tree array = TREE_OPERAND(lhs, 0);
            if (TREE_CODE(array) == MEM_REF || TREE_CODE(array) == INDIRECT_REF) {
                tree ptr = TREE_OPERAND(array, 0);
                gimple *def_stmt = find_pointer_source(ptr, info);
                if (def_stmt) {
                    info.member_stmt = def_stmt;
                    info.element_stmt = stmt;
                    info.acc_type = ACCESS_WRITE;
                    collected_accesses.push_back(info);
                }
            }
        }
        
        if (TREE_CODE(rhs1) == ARRAY_REF) {
            tree array = TREE_OPERAND(rhs1, 0);
            if (TREE_CODE(array) == MEM_REF || TREE_CODE(array) == INDIRECT_REF) {
                tree ptr = TREE_OPERAND(array, 0);
                gimple *def_stmt = find_pointer_source(ptr, info);
                if (def_stmt) {
                    info.member_stmt = def_stmt;
                    info.element_stmt = stmt;
                    info.acc_type = ACCESS_READ;
                    collected_accesses.push_back(info);
                }
            }
        }
    }
    // 处理函数调用中的参数
    else if (code == GIMPLE_CALL) {
        for (unsigned i = 0; i < gimple_call_num_args(stmt); i++) {
            tree arg = gimple_call_arg(stmt, i);
            if (TREE_CODE(arg) == ADDR_EXPR) {
                tree ref = TREE_OPERAND(arg, 0);
                if (TREE_CODE(ref) == MEM_REF || TREE_CODE(ref) == INDIRECT_REF) {
                    tree ptr = TREE_OPERAND(ref, 0);
                    gimple *def_stmt = find_pointer_source(ptr, info);
                    if (def_stmt) {
                        info.member_stmt = def_stmt;
                        info.element_stmt = stmt;
                        info.acc_type = ACCESS_RW;  // 函数调用可能读写
                        collected_accesses.push_back(info);
                    }
                }
            }
        }
    }
}

// IPA Pass 执行函数
static unsigned int double_pointer_access_pass_execute(function *fun) {
    basic_block bb;
    
    // 遍历所有基本块
    FOR_EACH_BB_FN(bb, fun) {
        gimple_stmt_iterator gsi;
        
        // 遍历基本块中的所有语句
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            analyze_gimple_stmt(stmt, fun);
        }
    }
    
    return 0;
}

// Pass 定义
namespace {

const pass_data double_pointer_access_pass_data = {
    IPA_PASS,                          // type
    "double_pointer_access",          // name
    OPTGROUP_NONE,                     // optinfo_flags
    TV_NONE,                           // tv_id
    0,                                 // properties_required
    0,                                 // properties_provided
    0,                                 // properties_destroyed
    0,                                 // todo_flags_start
    0                                  // todo_flags_finish
};

class double_pointer_access_pass : public ipa_opt_pass_d {
public:
    double_pointer_access_pass(gcc::context *ctxt)
        : ipa_opt_pass_d(double_pointer_access_pass_data, ctxt,
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

    unsigned int execute(function *fun) override {
        if (fun)
            return double_pointer_access_pass_execute(fun);
        
        // IPA模式：遍历所有函数
        struct cgraph_node *node;
        FOR_EACH_FUNCTION(node) {
            if (node->has_gimple_body_p()) {
                function *fn = DECL_STRUCT_FUNCTION(node->decl);
                if (fn) {
                    push_cfun(fn);
                    double_pointer_access_pass_execute(fn);
                    pop_cfun();
                }
            }
        }
        return 0;
    }
    
    bool gate(function *) override {
        return true;
    }
};

} // namespace

// 插件结束时打印收集的信息
static void plugin_finish_handler(void *gcc_data, void *user_data) {
    std::cout << "\n=== Double Pointer Access Analysis Results ===" << std::endl;
    std::cout << "Total accesses found: " << collected_accesses.size() << std::endl;
    
    for (size_t i = 0; i < collected_accesses.size(); i++) {
        const auto &access = collected_accesses[i];
        std::cout << "\n--- Access #" << (i + 1) << " ---" << std::endl;
        std::cout << "Function: " << access.func_name << std::endl;
        std::cout << "Struct Type: " << get_type_name(access.struct_type) << std::endl;
        std::cout << "Field Name: " << get_field_name(access.field_decl) << std::endl;
        std::cout << "Element Type: " << get_type_name(access.element_type) << std::endl;
        std::cout << "Access Type: " << 
            (access.acc_type == ACCESS_READ ? "READ" : 
             access.acc_type == ACCESS_WRITE ? "WRITE" : "READ/WRITE") << std::endl;
        
        if (access.loc != UNKNOWN_LOCATION) {
            expanded_location xloc = expand_location(access.loc);
            if (xloc.file)
                std::cout << "Location: " << xloc.file << ":" << xloc.line << ":" << xloc.column << std::endl;
        }
    }
    
    std::cout << "\n=== End of Analysis ===" << std::endl;
}

// 插件初始化
int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        std::cerr << "Plugin version mismatch!" << std::endl;
        return 1;
    }
    
    // 保存插件名称
    plugin_name = plugin_info->base_name;
    
    // 注册pass
    struct register_pass_info pass_info;
    pass_info.pass = new double_pointer_access_pass(g);
    pass_info.reference_pass_name = "whole-program";
    pass_info.ref_pass_instance_number = 0;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP,
                     NULL, &pass_info);
    
    // 注册结束回调
    register_callback(plugin_name, PLUGIN_FINISH,
                     plugin_finish_handler, NULL);
    
    std::cout << "Double Pointer Access Plugin loaded successfully." << std::endl;
    
    return 0;
}
