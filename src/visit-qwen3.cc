#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <gimple.h>
#include <tree-pass.h>

int plugin_is_GPL_compatible;

// PASS执行函数
static unsigned int execute_ptr_access_pass(void) {
    fprintf(stderr, "插件执行成功\n");
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

// 插件信息
