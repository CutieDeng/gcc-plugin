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
#include "symtab.h"
#include "tree-pass.h"

int plugin_is_GPL_compatible;

#define DEBUG_PRINT(fmt, ...) \
    printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)

// ===== 数据存储结构 =====
struct AccessTuple {
    tree struct_type;
    tree field_decl;
    tree element_type;
    gimple *element_stmt;
    int rw_mode; // 0 = read, 1 = write
};

// ===== 分析器类 =====
class Analyzer {
public:
    auto_vec<AccessTuple> results;

    long long analyze_function(cgraph_node *node) {
        if (!gimple_has_body_p(node->decl))
            return 0;

        push_cfun(DECL_STRUCT_FUNCTION(node->decl));
        basic_block bb;
        FOR_EACH_BB_FN(bb, cfun) {
            for (gimple_stmt_iterator gsi = gsi_start_bb(bb);
                 !gsi_end_p(gsi); gsi_next(&gsi)) {
                gimple *stmt = gsi_stmt(gsi);
                analyze_stmt(stmt);
            }
        }
        pop_cfun();
        return 0;
    }

    void analyze_stmt(gimple *stmt) {
        if (is_gimple_assign(stmt)) {
            tree lhs = gimple_assign_lhs(stmt);
            tree rhs = gimple_assign_rhs1(stmt);

            if (lhs && TREE_CODE(lhs) == ARRAY_REF) {
                AccessTuple tup {};
                tup.struct_type = TREE_TYPE(TREE_OPERAND(lhs, 0));
                tup.element_type = TREE_TYPE(lhs);
                tup.element_stmt = stmt;
                tup.rw_mode = 1; // 写
                results.safe_push(tup);
            }
            if (rhs && TREE_CODE(rhs) == ARRAY_REF) {
                AccessTuple tup {};
                tup.struct_type = TREE_TYPE(TREE_OPERAND(rhs, 0));
                tup.element_type = TREE_TYPE(rhs);
                tup.element_stmt = stmt;
                tup.rw_mode = 0; // 读
                results.safe_push(tup);
            }
        }
    }

    void dump_results() {
        for (unsigned i = 0; i < results.length(); ++i) {
            AccessTuple &t = results[i];
            printf("==== Access Tuple ====\n");
            if (t.struct_type)
                printf("StructType: %s\n", get_tree_code_name(TREE_CODE(t.struct_type)));
            if (t.element_type)
                printf("ElementType: %s\n", get_tree_code_name(TREE_CODE(t.element_type)));
            printf("RW Mode: %d\n", t.rw_mode);
            printf("Stmt:\n");
            print_gimple_stmt(stdout, t.element_stmt, 0, TDF_NONE);
            printf("\n");
        }
    }
};

// ===== IPA Pass =====
namespace {

const pass_data ipa_pass_data = {
    IPA_PASS,                      // type
    "ptr_access_analyzer",         // name
    OPTGROUP_NONE,                 // optinfo_flags
    TV_NONE,                       // tv_id
    PROP_gimple_any,               // properties_required
    0, 0, 0, 0                     // provided/destroyed/todo_flags
};

struct ipa_analyzer_pass : simple_ipa_opt_pass {
    ipa_analyzer_pass(gcc::context *ctxt)
        : simple_ipa_opt_pass(ipa_pass_data, ctxt) {}

    unsigned int execute(function *) override { return 0; }

    void ipa_execute() override {
        Analyzer analyzer;
        FOR_EACH_DEFINED_FUNCTION(node) {
            if (!gimple_has_body_p(node->decl)) continue;
            analyzer.analyze_function(node);
        }
        analyzer.dump_results();
    }
};

} // namespace

// ===== 插件入口 =====
int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version) {

    if (!plugin_default_version_check(version, &gcc_version)) {
        printf("Incompatible gcc/plugin versions\n");
        return 1;
    }

    static ipa_analyzer_pass mypass(g);
    struct register_pass_info pass_info;
    pass_info.pass = &mypass;
    pass_info.reference_pass_name = "whole-program";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;

    register_callback(plugin_info->base_name,
                      PLUGIN_PASS_MANAGER_SETUP,
                      nullptr,
                      &pass_info);

    printf("Plugin %s loaded.\n", plugin_info->base_name);
    return 0;
}
