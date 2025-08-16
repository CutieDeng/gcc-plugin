#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <gimple-walk.h>
#include <cgraph.h>
#include <tree-cfg.h>
#include <stringpool.h>
#include <attribs.h>
#include <print-tree.h>
#include <gimple-pretty-print.h>
#include <tree-inline.h>
#include <langhooks.h>
#include <diagnostic-core.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <set>
#include <map>

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
    std::string access_desc;    // 访问描述
    bool is_stl_container;      // 是否是STL容器
    
    access_info() : struct_type(NULL_TREE), field_decl(NULL_TREE), 
                   element_type(NULL_TREE), member_stmt(NULL), 
                   element_stmt(NULL), acc_type(ACCESS_READ), 
                   loc(UNKNOWN_LOCATION), func_name(""), 
                   access_desc(""), is_stl_container(false) {}
};

// 全局收集器
static std::vector<access_info> collected_accesses;
static const char *plugin_name = "double_pointer_access";

// STL容器类型识别
static std::set<std::string> stl_containers = {
    "vector", "deque", "list", "forward_list",
    "set", "map", "multiset", "multimap",
    "unordered_set", "unordered_map",
    "unordered_multiset", "unordered_multimap",
    "array", "string", "basic_string"
};

// 辅助函数：获取类型名称
static std::string get_type_name(tree type) {
    if (!type) return "<unknown>";
    
    // 处理引用类型
    if (TREE_CODE(type) == REFERENCE_TYPE) {
        return "reference to " + get_type_name(TREE_TYPE(type));
    }
    
    if (TYPE_NAME(type)) {
        if (TREE_CODE(TYPE_NAME(type)) == IDENTIFIER_NODE) {
            return IDENTIFIER_POINTER(TYPE_NAME(type));
        } else if (TREE_CODE(TYPE_NAME(type)) == TYPE_DECL) {
            tree decl_name = DECL_NAME(TYPE_NAME(type));
            if (decl_name) {
                std::string name = IDENTIFIER_POINTER(decl_name);
                // 检查是否是模板实例
                if (TREE_CODE(type) == RECORD_TYPE && TYPE_CONTEXT(type)) {
                    tree context = TYPE_CONTEXT(type);
                    if (TREE_CODE(context) == NAMESPACE_DECL) {
                        tree ns_name = DECL_NAME(context);
                        if (ns_name) {
                            std::string ns = IDENTIFIER_POINTER(ns_name);
                            if (ns == "std" || ns == "__gnu_cxx") {
                                return "std::" + name;
                            }
                        }
                    }
                }
                return name;
            }
        }
    }
    
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
            return TYPE_PRECISION(type) == 32 ? "float" : "double";
        case POINTER_TYPE:
            return "pointer to " + get_type_name(TREE_TYPE(type));
        case ARRAY_TYPE:
            return "array of " + get_type_name(TREE_TYPE(type));
        case RECORD_TYPE:
            return "struct/class";
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

// 检查是否是STL容器
static bool is_stl_container_type(tree type) {
    if (!type || TREE_CODE(type) != RECORD_TYPE)
        return false;
    
    std::string type_name = get_type_name(type);
    
    // 检查是否包含STL容器名称
    for (const auto& container : stl_containers) {
        if (type_name.find(container) != std::string::npos) {
            return true;
        }
    }
    
    // 检查是否有典型的STL容器成员
    for (tree field = TYPE_FIELDS(type); field; field = DECL_CHAIN(field)) {
        if (TREE_CODE(field) == FIELD_DECL) {
            std::string fname = get_field_name(field);
            // libstdc++ 的 vector 实现使用 _M_impl 结构
            if (fname == "_M_impl" || fname == "_M_start" || 
                fname == "_M_finish" || fname == "_M_data") {
                return true;
            }
        }
    }
    
    return false;
}

// 分析STL容器的指针字段
static void analyze_stl_container_fields(tree type, access_info &info) {
    if (!type || TREE_CODE(type) != RECORD_TYPE)
        return;
    
    // 遍历所有字段
    for (tree field = TYPE_FIELDS(type); field; field = DECL_CHAIN(field)) {
        if (TREE_CODE(field) == FIELD_DECL) {
            tree field_type = TREE_TYPE(field);
            std::string fname = get_field_name(field);
            
            // 查找指针类型的数据成员
            if (TREE_CODE(field_type) == POINTER_TYPE) {
                if (fname == "_M_start" || fname == "_M_data" || 
                    fname == "_M_ptr" || fname == "_M_p") {
                    info.field_decl = field;
                    info.element_type = TREE_TYPE(field_type);
                    info.is_stl_container = true;
                    return;
                }
            }
            
            // 递归检查嵌套结构（如 _M_impl）
            if (TREE_CODE(field_type) == RECORD_TYPE) {
                if (fname == "_M_impl" || fname == "_M_data_plus") {
                    analyze_stl_container_fields(field_type, info);
                    if (info.field_decl) {
                        return;
                    }
                }
            }
        }
    }
}

// 检查是否是二级指针访问（增强版）
static bool is_double_pointer_access_enhanced(tree expr, access_info &info) {
    if (!expr) return false;
    
    // 原有的COMPONENT_REF处理
    if (TREE_CODE(expr) == COMPONENT_REF) {
        tree base = TREE_OPERAND(expr, 0);
        tree field = TREE_OPERAND(expr, 1);
        
        // 处理 this->field 或 obj.field
        tree base_type = TREE_TYPE(base);
        if (TREE_CODE(base_type) == RECORD_TYPE) {
            // 直接对象访问
            if (is_stl_container_type(base_type)) {
                info.struct_type = base_type;
                analyze_stl_container_fields(base_type, info);
                return info.field_decl != NULL_TREE;
            }
        }
        
        // 处理间接引用
        if (TREE_CODE(base) == INDIRECT_REF || TREE_CODE(base) == MEM_REF) {
            tree ptr_expr = TREE_OPERAND(base, 0);
            tree ptr_type = TREE_TYPE(ptr_expr);
            
            if (TREE_CODE(ptr_type) == POINTER_TYPE) {
                tree struct_type = TREE_TYPE(ptr_type);
                if (TREE_CODE(struct_type) == RECORD_TYPE) {
                    // 检查是否是STL容器
                    if (is_stl_container_type(struct_type)) {
                        info.struct_type = struct_type;
                        info.is_stl_container = true;
                        analyze_stl_container_fields(struct_type, info);
                        return info.field_decl != NULL_TREE;
                    }
                    
                    // 原有的指针字段检查
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
    
    // 处理 MEM_REF 直接访问
    if (TREE_CODE(expr) == MEM_REF) {
        tree ptr = TREE_OPERAND(expr, 0);
        if (TREE_CODE(ptr) == SSA_NAME) {
            tree ptr_type = TREE_TYPE(ptr);
            if (TREE_CODE(ptr_type) == POINTER_TYPE) {
                tree pointed_type = TREE_TYPE(ptr_type);
                // 检查是否访问了STL容器的内部数据
                if (is_stl_container_type(pointed_type)) {
                    info.struct_type = pointed_type;
                    info.is_stl_container = true;
                    analyze_stl_container_fields(pointed_type, info);
                    return info.field_decl != NULL_TREE;
                }
            }
        }
    }
    
    return false;
}

// 递归查找SSA定义链（增强版）
static gimple* find_pointer_source_enhanced(tree ptr, access_info &info, int depth = 0) {
    if (depth > 10) // 防止无限递归
        return NULL;
        
    if (TREE_CODE(ptr) != SSA_NAME)
        return NULL;
    
    gimple *def_stmt = SSA_NAME_DEF_STMT(ptr);
    if (!def_stmt)
        return NULL;
    
    if (gimple_code(def_stmt) == GIMPLE_ASSIGN) {
        tree rhs = gimple_assign_rhs1(def_stmt);
        
        // 检查是否是二级指针访问
        if (is_double_pointer_access_enhanced(rhs, info)) {
            return def_stmt;
        }
        
        // 处理类型转换
        if (CONVERT_EXPR_P(rhs)) {
            tree inner = TREE_OPERAND(rhs, 0);
            if (TREE_CODE(inner) == SSA_NAME) {
                return find_pointer_source_enhanced(inner, info, depth + 1);
            }
        }
        
        // 继续递归查找
        if (TREE_CODE(rhs) == SSA_NAME) {
            return find_pointer_source_enhanced(rhs, info, depth + 1);
        }
        
        // 处理指针算术
        if (gimple_assign_rhs_code(def_stmt) == POINTER_PLUS_EXPR) {
            tree base = gimple_assign_rhs1(def_stmt);
            if (TREE_CODE(base) == SSA_NAME) {
                return find_pointer_source_enhanced(base, info, depth + 1);
            }
        }
    } else if (gimple_code(def_stmt) == GIMPLE_PHI) {
        // 处理PHI节点
        gphi *phi = as_a<gphi*>(def_stmt);
        for (unsigned i = 0; i < gimple_phi_num_args(phi); i++) {
            tree arg = gimple_phi_arg_def(phi, i);
            gimple *source = find_pointer_source_enhanced(arg, info, depth + 1);
            if (source)
                return source;
        }
    } else if (gimple_code(def_stmt) == GIMPLE_CALL) {
        // 处理函数调用返回值
        tree fn = gimple_call_fndecl(def_stmt);
        if (fn && DECL_NAME(fn)) {
            const char *fname = IDENTIFIER_POINTER(DECL_NAME(fn));
            // 检查是否是容器的数据访问函数
            if (strstr(fname, "data") || strstr(fname, "begin") || 
                strstr(fname, "_M_data")) {
                // 获取this指针（第一个参数）
                if (gimple_call_num_args(def_stmt) > 0) {
                    tree this_arg = gimple_call_arg(def_stmt, 0);
                    tree this_type = TREE_TYPE(this_arg);
                    if (TREE_CODE(this_type) == POINTER_TYPE) {
                        tree class_type = TREE_TYPE(this_type);
                        if (is_stl_container_type(class_type)) {
                            info.struct_type = class_type;
                            info.is_stl_container = true;
                            analyze_stl_container_fields(class_type, info);
                            return def_stmt;
                        }
                    }
                }
            }
        }
    }
    
    return NULL;
}

// 分析函数调用中的STL容器访问
static void analyze_call_stmt(gcall *call_stmt, function *fun) {
    tree fn = gimple_call_fndecl(call_stmt);
    if (!fn || !DECL_NAME(fn)) return;
    
    const char *fname = IDENTIFIER_POINTER(DECL_NAME(fn));
    
    // 检查是否是STL容器的成员函数
    if (strstr(fname, "at") || strstr(fname, "operator[]") || 
        strstr(fname, "front") || strstr(fname, "back") ||
        strstr(fname, "data") || strstr(fname, "begin")) {
        
        // 获取this指针（通常是第一个参数）
        if (gimple_call_num_args(call_stmt) > 0) {
            tree this_arg = gimple_call_arg(call_stmt, 0);
            tree this_type = TREE_TYPE(this_arg);
            
            if (TREE_CODE(this_type) == POINTER_TYPE || 
                TREE_CODE(this_type) == REFERENCE_TYPE) {
                tree class_type = TREE_TYPE(this_type);
                
                if (is_stl_container_type(class_type)) {
                    access_info info;
                    info.struct_type = class_type;
                    info.is_stl_container = true;
                    info.func_name = function_name(fun);
                    info.loc = gimple_location(call_stmt);
                    info.element_stmt = call_stmt;
                    info.acc_type = ACCESS_READ; // 默认为读
                    
                    // 分析容器的内部结构
                    analyze_stl_container_fields(class_type, info);
                    
                    if (info.field_decl || info.is_stl_container) {
                        info.access_desc = std::string("STL container access via ") + fname;
                        collected_accesses.push_back(info);
                    }
                }
            }
        }
    }
}

// 分析单个gimple语句（增强版）
static void analyze_gimple_stmt_enhanced(gimple *stmt, function *fun) {
    if (!stmt) return;
    
    access_info info;
    info.loc = gimple_location(stmt);
    info.func_name = function_name(fun);
    
    enum gimple_code code = gimple_code(stmt);
    
    // 处理函数调用
    if (code == GIMPLE_CALL) {
        analyze_call_stmt(as_a<gcall*>(stmt), fun);
        
        // 继续原有的调用参数分析
        for (unsigned i = 0; i < gimple_call_num_args(stmt); i++) {
            tree arg = gimple_call_arg(stmt, i);
            if (TREE_CODE(arg) == ADDR_EXPR) {
                tree ref = TREE_OPERAND(arg, 0);
                if (TREE_CODE(ref) == MEM_REF || TREE_CODE(ref) == INDIRECT_REF) {
                    tree ptr = TREE_OPERAND(ref, 0);
                    gimple *def_stmt = find_pointer_source_enhanced(ptr, info);
                    if (def_stmt) {
                        info.member_stmt = def_stmt;
                        info.element_stmt = stmt;
                        info.acc_type = ACCESS_RW;
                        collected_accesses.push_back(info);
                    }
                }
            }
        }
        return;
    }
    
    // 处理赋值语句
    if (code == GIMPLE_ASSIGN) {
        tree lhs = gimple_assign_lhs(stmt);
        tree rhs1 = gimple_assign_rhs1(stmt);
        
        // 检查左值（写操作）
        if (TREE_CODE(lhs) == MEM_REF || TREE_CODE(lhs) == INDIRECT_REF) {
            tree ptr = TREE_OPERAND(lhs, 0);
            gimple *def_stmt = find_pointer_source_enhanced(ptr, info);
            if (def_stmt) {
                info.member_stmt = def_stmt;
                info.element_stmt = stmt;
                info.acc_type = ACCESS_WRITE;
                info.access_desc = "Direct memory write";
                collected_accesses.push_back(info);
            }
        }
        
        // 检查右值（读操作）
        if (TREE_CODE(rhs1) == MEM_REF || TREE_CODE(rhs1) == INDIRECT_REF) {
            tree ptr = TREE_OPERAND(rhs1, 0);
            gimple *def_stmt = find_pointer_source_enhanced(ptr, info);
            if (def_stmt) {
                info.member_stmt = def_stmt;
                info.element_stmt = stmt;
                info.acc_type = ACCESS_READ;
                info.access_desc = "Direct memory read";
                collected_accesses.push_back(info);
            }
        }
        
        // 处理数组访问
        if (TREE_CODE(lhs) == ARRAY_REF) {
            tree array = TREE_OPERAND(lhs, 0);
            if (TREE_CODE(array) == MEM_REF || TREE_CODE(array) == INDIRECT_REF) {
                tree ptr = TREE_OPERAND(array, 0);
                gimple *def_stmt = find_pointer_source_enhanced(ptr, info);
                if (def_stmt) {
                    info.member_stmt = def_stmt;
                    info.element_stmt = stmt;
                    info.acc_type = ACCESS_WRITE;
                    info.access_desc = "Array element write";
                    collected_accesses.push_back(info);
                }
            }
        }
        
        if (TREE_CODE(rhs1) == ARRAY_REF) {
            tree array = TREE_OPERAND(rhs1, 0);
            if (TREE_CODE(array) == MEM_REF || TREE_CODE(array) == INDIRECT_REF) {
                tree ptr = TREE_OPERAND(array, 0);
                gimple *def_stmt = find_pointer_source_enhanced(ptr, info);
                if (def_stmt) {
                    info.member_stmt = def_stmt;
                    info.element_stmt = stmt;
                    info.acc_type = ACCESS_READ;
                    info.access_desc = "Array element read";
                    collected_accesses.push_back(info);
                }
            }
        }
        
        // 处理COMPONENT_REF（结构体成员访问）
        if (TREE_CODE(lhs) == COMPONENT_REF || TREE_CODE(rhs1) == COMPONENT_REF) {
            tree comp_ref = (TREE_CODE(lhs) == COMPONENT_REF) ? lhs : rhs1;
            if (is_double_pointer_access_enhanced(comp_ref, info)) {
                info.element_stmt = stmt;
                info.acc_type = (TREE_CODE(lhs) == COMPONENT_REF) ? ACCESS_WRITE : ACCESS_READ;
                info.access_desc = "Component reference access";
                collected_accesses.push_back(info);
            }
        }
    }
}

// IPA Pass 执行函数
static unsigned int double_pointer_access_pass_execute(function *fun) {
    basic_block bb;
    
    // 确保函数体存在
    if (!fun || !fun->cfg)
        return 0;
    
    // 遍历所有基本块
    FOR_EACH_BB_FN(bb, fun) {
        gimple_stmt_iterator gsi;
        
        // 遍历基本块中的所有语句
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            analyze_gimple_stmt_enhanced(stmt, fun);
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

// 打印 gimple 语句
static void print_gimple_stmt_info(gimple *stmt) {
    if (!stmt) {
        std::cout << "  <null statement>" << std::endl;
        return;
    }
    
    print_gimple_stmt(stdout, stmt, 0, TDF_SLIM);
}

// 插件结束时打印收集的信息
static void plugin_finish_handler(void *gcc_data, void *user_data) {
    std::cout << "\n=== Double Pointer Access Analysis Results ===" << std::endl;
    std::cout << "Total accesses found: " << collected_accesses.size() << std::endl;
    
    // 统计STL容器访问
    int stl_count = 0;
    for (const auto &access : collected_accesses) {
        if (access.is_stl_container)
            stl_count++;
    }
    std::cout << "STL container accesses: " << stl_count << std::endl;
    
    for (size_t i = 0; i < collected_accesses.size(); i++) {
        const auto &access = collected_accesses[i];
        std::cout << "\n--- Access #" << (i + 1) << " ---" << std::endl;
        std::cout << "Function: " << access.func_name << std::endl;
        std::cout << "Struct/Class Type: " << get_type_name(access.struct_type);
        if (access.is_stl_container) {
            std::cout << " [STL Container]";
        }
        std::cout << std::endl;
        
        if (access.field_decl) {
            std::cout << "Field Name: " << get_field_name(access.field_decl) << std::endl;
        }
        
        if (access.element_type) {
            std::cout << "Element Type: " << get_type_name(access.element_type) << std::endl;
        }
        
        std::cout << "Access Type: " << 
            (access.acc_type == ACCESS_READ ? "READ" : 
             access.acc_type == ACCESS_WRITE ? "WRITE" : "READ/WRITE") << std::endl;
        
        if (!access.access_desc.empty()) {
            std::cout << "Description: " << access.access_desc << std::endl;
        }
        
        if (access.loc != UNKNOWN_LOCATION) {
            expanded_location xloc = expand_location(access.loc);
            if (xloc.file)
                std::cout << "Location: " << xloc.file << ":" << xloc.line << ":" << xloc.column << std::endl;
        }
        
        if (access.member_stmt) {
            std::cout << "Member access statement: ";
            print_gimple_stmt_info(access.member_stmt);
        }
        
        if (access.element_stmt) {
            std::cout << "Element access statement: ";
            print_gimple_stmt_info(access.element_stmt);
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
    
    // 注册pass - 在优化之后运行以获得内联后的代码
    struct register_pass_info pass_info;
    pass_info.pass = new double_pointer_access_pass(g);
    pass_info.reference_pass_name = "optimized";  // 在优化后运行
    pass_info.ref_pass_instance_number = 1;       // 使用1表示唯一实例
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP,
                     NULL, &pass_info);
    
    // 注册结束回调
    register_callback(plugin_name, PLUGIN_FINISH,
                     plugin_finish_handler, NULL);
    
    std::cout << "Enhanced Double Pointer Access Plugin (with STL support) loaded successfully." << std::endl;
    
    return 0;
}
