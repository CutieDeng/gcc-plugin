#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-pretty-print.h"
#include "context.h"
#include "function.h"
#include "print-tree.h"
#include "tree-pretty-print.h"
#include "cgraph.h"
#include "diagnostic.h"
#include "stringpool.h"
#include "vec.h"
#include "hash-table.h"

int plugin_is_GPL_compatible;

#define DEBUG_PRINT(fmt, ...) \
    printf("[DEBUG][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

// ===== 错误码与宏定义 =====
enum AnalyzerErr {
    ERR_NONE                = 0,
    ERR_FRAMEWORK_LOGIC     = 1 << 0,
    ERR_SOFT_ASSERT         = 1 << 1,
    ERR_LOGIC               = 1 << 2,
    ERR_MEMORY              = 1 << 3
};

#define TRY(x) do { long long _e = (x); if (_e) { \
    DEBUG_PRINT("TRY failed at %s:%d -> code=%lld", __FILE__, __LINE__, _e); \
    return _e; } } while (0)

#define TRY_WEAK(x) do { long long _e = (x); if (_e && _e != ERR_SOFT_ASSERT) { \
    DEBUG_PRINT("TRY_WEAK failed at %s:%d -> code=%lld", __FILE__, __LINE__, _e); \
    return _e; } } while (0)


// ===== 数据存储结构 =====
struct AccessTuple {
    tree struct_type;
    tree field_decl;
    tree element_type;
    gimple *member_stmt;
    gimple *element_stmt;
    int rw_mode; // 0 = read, 1 = write
};

// ===== 分析器类 =====
class Analyzer {
public:
    auto_vec<AccessTuple> results;

    Analyzer() {}
    ~Analyzer() {}

    long long analyze_function(cgraph_node *node) {
        if (!gimple_has_body_p(node->decl))
            return ERR_NONE;

        push_cfun(DECL_STRUCT_FUNCTION(node->decl));
        basic_block bb;
        FOR_EACH_BB_FN(bb, cfun) {
            for (gimple_stmt_iterator gsi = gsi_start_bb(bb);
                 !gsi_end_p(gsi); gsi_next(&gsi)) {
                gimple *stmt = gsi_stmt(gsi);
                TRY_WEAK(analyze_stmt(stmt));
            }
        }
        pop_cfun();
        return ERR_NONE;
    }

    long long analyze_stmt(gimple *stmt) {
        // 寻找 ARRAY_REF 或 COMPONENT_REF 嵌套 MEM_REF
        if (is_gimple_assign(stmt)) {
            tree lhs = gimple_assign_lhs(stmt);
            tree rhs = gimple_assign_rhs1(stmt);

            if (TREE_CODE(lhs) == ARRAY_REF || TREE_CODE(rhs) == ARRAY_REF) {
                tree arr = (TREE_CODE(lhs) == ARRAY_REF ? lhs : rhs);
                tree base = TREE_OPERAND(arr, 0);

                if (TREE_CODE(base) == COMPONENT_REF &&
                    TREE_CODE(TREE_OPERAND(base, 0)) == INDIRECT_REF) {
                    tree ind = TREE_OPERAND(base, 0); // INDIRECT_REF
                    tree comp = base;
                    tree structtype = TREE_TYPE(TREE_OPERAND(ind, 0));
                    tree field = TREE_OPERAND(comp, 1);
                    tree elemtype = TREE_TYPE(arr);

                    AccessTuple tup;
                    tup.struct_type = structtype;
                    tup.field_decl = field;
                    tup.element_type = elemtype;
                    tup.member_stmt = stmt;
                    tup.element_stmt = stmt;
                    tup.rw_mode = classify_rw(stmt, lhs, rhs);

                    results.safe_push(tup);
                }
            }
        }
        return ERR_NONE;
    }

    int classify_rw(gimple *stmt, tree lhs, tree rhs) {
        // 简单区分：如果 ARRAY_REF 在 lhs，则写，否则读
        if (TREE_CODE(lhs) == ARRAY_REF)
            return 1;
        return 0;
    }

    void dump_results() {
        for (unsigned i = 0; i < results.length(); ++i) {
            AccessTuple &t = results[i];
            printf("==== Access Tuple ====\n");
            printf("StructType: %s\n", t->struct_type ? get_tree_code_name(TREE_CODE(t->struct_type)) : "(null)");
            printf("Field: %s\n", t->field_decl ? get_tree_code_name(TREE_CODE(t->field_decl)) : "(null)");
            printf("ElementType: %s\n", t->element_type ? get_tree_code_name(TREE_CODE(t->element_type)) : "(null)");
            printf("RW Mode: %d\n", t->rw_mode);
            printf("Stmt:\n");
            print_gimple_stmt(stdout, t->element_stmt, 0, 0);
            printf("\n");
        }
    }
};


// ===== IPA Pass 定义 =====
namespace {
const pass_data ipa_pass_data = {
    IPA_PASS, /* type */
    "ptr_access_analyzer", /* name */
    OPTGROUP_NONE, /* optinfo_flags */
    TV_NONE, /* tv_id */
    PROP_gimple_any, /* properties_required */
    0, /* properties_provided */
    0, /* properties_destroyed */
    0, /* todo_flags_start */
    0 /* todo_flags_finish */
};

struct ipa_analyzer_pass : ipa_opt_pass_d {
    ipa_analyzer_pass(gcc::context *ctxt)
        : ipa_opt_pass_d(ipa_pass_data, ctxt, nullptr, true) {}

    virtual unsigned int execute(function *fn) override {
        // 不在 function 内部执行，放在 whole-program ipa_execute
        return 0;
    }

    virtual void ipa_execute(cgraph_node *) override {
        Analyzer analyzer;
        for (cgraph_node *node = cgraph_nodes; node; node = node->next) {
            if (!gimple_has_body_p(node->decl))
                continue;
            analyzer.analyze_function(node);
        }
        analyzer.dump_results();
    }

    virtual bool gate(function *) override { return true; }
};
} // namespace

// ===== 插件入口 =====
int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        printf("Incompatible gcc/plugin versions\n");
        return 1;
    }

    struct register_pass_info pass_info;
    pass_info.pass = new ipa_analyzer_pass(g);
    pass_info.reference_pass_name = "whole-program";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;

    register_callback(plugin_info->base_name,
                      PLUGIN_PASS_MANAGER_SETUP,
                      nullptr,
                      &pass_info);

    printf("Plugin %s loaded\n", plugin_info->base_name);
    return 0;
}
