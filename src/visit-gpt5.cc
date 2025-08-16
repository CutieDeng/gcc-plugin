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
#include "symbol-summary.h"
#include "ipa-pass.h"

int plugin_is_GPL_compatible;

#define DEBUG_PRINT(fmt, ...) \
    printf("[DEBUG][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

// ===== 错误码定义 =====
enum AnalyzerErr {
    ERR_NONE                = 0,
    ERR_FRAMEWORK_LOGIC     = 1 << 0,
    ERR_SOFT_ASSERT         = 1 << 1,
    ERR_LOGIC               = 1 << 2,
    ERR_MEMORY              = 1 << 3
};

#define TRY(x) do { long long _e = (x); if (_e) { \
    DEBUG_PRINT("TRY failed -> code=%lld", _e); \
    return _e; } } while (0)

#define TRY_WEAK(x) do { long long _e = (x); if (_e && _e != ERR_SOFT_ASSERT) { \
    DEBUG_PRINT("TRY_WEAK failed -> code=%lld", _e); \
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
        if (is_gimple_assign(stmt)) {
            tree lhs = gimple_assign_lhs(stmt);
            tree rhs = gimple_assign_rhs1(stmt);

            if (TREE_CODE(lhs) == ARRAY_REF || TREE_CODE(rhs) == ARRAY_REF) {
                tree arr = (TREE_CODE(lhs) == ARRAY_REF ? lhs : rhs);
                tree base = TREE_OPERAND(arr, 0);

                if (TREE_CODE(base) == COMPONENT_REF &&
                    TREE_CODE(TREE_OPERAND(base, 0)) == INDIRECT_REF) {
                    tree ind = TREE_OPERAND(base, 0);
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
                    tup.rw_mode = classify_rw(lhs, rhs);

                    results.safe_push(tup);
                }
            }
        }
        return ERR_NONE;
    }

    int classify_rw(tree lhs, tree rhs) {
        if (TREE_CODE(lhs) == ARRAY_REF)
            return 1;
        return 0;
    }

    void dump_results() {
        for (unsigned i = 0; i < results.length(); ++i) {
            AccessTuple &t = results[i];
            printf("==== Access Tuple ====\n");
            if (t.struct_type) {
                printf("StructType: %s\n", get_tree_code_name(TREE_CODE(t.struct_type)));
            }
            if (t.field_decl) {
                printf("Field: %s\n", get_tree_code_name(TREE_CODE(t.field_decl)));
            }
            if (t.element_type) {
                printf("ElementType: %s\n", get_tree_code_name(TREE_CODE(t.element_type)));
            }
            printf("RW Mode: %d\n", t.rw_mode);
            printf("Stmt:\n");
            print_gimple_stmt(stdout, t.element_stmt, 0, 0);
            printf("\n");
        }
    }
};

// ===== IPA Pass =====
namespace {
const ipa_opt_pass_data ipa_pass_data = {
    .type = IPA_PASS,
    .name = "ptr_access_analyzer",
    .optinfo_flags = OPTGROUP_NONE,
    .tv_id = TV_NONE,
    .properties_required = PROP_gimple_any,
    .properties_provided = 0,
    .properties_destroyed = 0,
    .todo_flags_start = 0,
    .todo_flags_finish = 0,
};

struct ipa_analyzer_pass : simple_ipa_opt_pass {
    ipa_analyzer_pass(gcc::context *ctxt)
        : simple_ipa_opt_pass(ipa_pass_data, ctxt) {}

    unsigned int execute(function *) override { return 0; }

    void ipa_execute(cgraph_node *) override {
        Analyzer analyzer;
        FOR_EACH_DEFINED_FUNCTION(cnode) {
            if (!gimple_has_body_p(cnode->decl))
                continue;
            analyzer.analyze_function(cnode);
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

    printf("Plugin %s loaded\n", plugin_info->base_name);
    return 0;
}
