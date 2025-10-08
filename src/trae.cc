#include <stdarg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// GCC插件头文件 - 这些头文件在编译时由build.rkt脚本提供正确的包含路径
#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <diagnostic.h>
#include <function.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <gimple-walk.h>
#include <stringpool.h>

// 插件ID声明，必须放在include之后
int plugin_is_GPL_compatible;

// 错误码定义
typedef long long error_code_t;

enum {
    ERROR_UNINIT = 0,
    ERROR_OK = 1,
    ERROR_RECOVERABLE = 2,
    ERROR_RECOVERABLE_1 = 3,
    ERROR_RECOVERABLE_2 = 4,
    ERROR_RECOVERABLE_3 = 5,
    ERROR_SELECT_OUT_OF_RANGE = 6,
    ERROR_GCC_ERROR = 7,
    ERROR_CUSTOM_LOGIC_ERROR = 8,
    ERROR_MEMORY_RESOURCE_ERROR = 9,
    ERROR_SYSTEM_RESOURCE_ERROR = 10,
    ERROR_SYSTEM_ERROR = 11,
    ERROR_INPUT_OUTPUT_ERROR = 12,
    ERROR_GARBAGE_COLLECTION_ERROR = 13
};

// 调试宏定义
#define CUTIE_DEBUG_PRINT_RAW(file, fmt_msg, ...) do { fprintf(file, "(%s:%d %s ", __FILE__, __LINE__, __func__); \
    fprintf(file, fmt_msg, ##__VA_ARGS__); \
    fprintf(file, ")\n"); \
} while (0)

// 上下文定义
typedef struct {
    error_code_t error_code;
    error_code_t sub_error_code;
    char error_message[1024];
    char const *custom_error_message;
    FILE *debug_file;
} CutieContext;

// 数据来源类型枚举
typedef enum {
    DATA_SOURCE_UNKNOWN = 0,
    DATA_SOURCE_DIRECT_FROM_RETURN = 1,
    DATA_SOURCE_NULLPTR = 2,
    DATA_SOURCE_LITERAL = 3,
    DATA_SOURCE_COMPLEX = 4
} DataSourceType;

// 指针来源统计结构体
typedef struct {
    long long direct_from_return_count;
    long long nullptr_count;
    long long literal_count;
    long long complex_count;
    long long total_count;
} PointerSourceStats;

// 业务上下文定义
typedef struct {
    CutieContext *cutie_context;
    bool initialized;
    PointerSourceStats pointer_stats;
} VibeBusinessContext;

// 全局上下文
CutieContext g_cutie_context;
VibeBusinessContext g_business_context;

// LTO模式标志 - 使用GCC框架的LTO检测
bool g_is_lto_mode = false;

// LTO链接阶段标志 - 标记是否处于LTO链接阶段
bool g_is_lto_link_phase = false;

// 函数前向声明
static void cutie_context_set_error(CutieContext *ctx, error_code_t error_code, error_code_t sub_error_code, char const *format, ...);
static void identify_vector_fields(tree type, tree *ptr_field, tree *size_field, tree *capacity_field);
static DataSourceType analyze_pointer_source(tree expr);
static void track_pointer_writes(gimple *stmt, VibeBusinessContext *ctx);
static void collect_field_data_sources(function *fn, VibeBusinessContext *ctx);
static void handle_template_instantiation_impl(tree t, VibeBusinessContext *ctx);
static error_code_t initialize_business_context(VibeBusinessContext *ctx, CutieContext *cutie_ctx, error_code_t *result);
static void cleanup_business_context(VibeBusinessContext *ctx);
static void register_callbacks_impl(plugin_name_args *plugin_info, void *user_data);

extern "C" void handle_template_instantiation(void *gcc_data, void *user_data);
extern "C" void register_callbacks(plugin_name_args *plugin_info, plugin_gcc_version *version);
extern "C" int plugin_init(plugin_name_args *plugin_info, plugin_gcc_version *version);
extern "C" void plugin_finish(void *gcc_data, void *user_data);

// 设置错误信息
static void cutie_context_set_error(CutieContext *ctx, error_code_t error_code, error_code_t sub_error_code, char const *format, ...) {
    if (!ctx) {
        // 如果没有上下文，使用标准错误输出
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, " (error code: %lld, sub error: %lld)\n", error_code, sub_error_code);
        return;
    }
    
    ctx->error_code = error_code;
    ctx->sub_error_code = sub_error_code;
    
    // 格式化错误消息
    va_list args;
    va_start(args, format);
    vsnprintf(ctx->error_message, sizeof(ctx->error_message), format, args);
    va_end(args);
    
    // 输出错误信息
    if (ctx->debug_file) {
        fprintf(ctx->debug_file, "(\"%s\" %lld %lld \"%s\")\n", 
                __FILE__, error_code, sub_error_code, ctx->error_message);
    } else {
        fprintf(stderr, "(\"%s\" %lld %lld \"%s\")\n", 
                __FILE__, error_code, sub_error_code, ctx->error_message);
    }
}

// 识别vector类中的字段
static void identify_vector_fields(tree type, tree *ptr_field, tree *size_field, tree *capacity_field) {
    if (!type || TREE_CODE(type) != RECORD_TYPE) {
        *ptr_field = NULL_TREE;
        *size_field = NULL_TREE;
        *capacity_field = NULL_TREE;
        return;
    }
    
    tree field;
    for (field = TYPE_FIELDS(type); field; field = TREE_CHAIN(field)) {
        if (TREE_CODE(field) != FIELD_DECL) continue;
        
        const char *field_name = IDENTIFIER_POINTER(DECL_NAME(field));
        
        // 简单的字段名称匹配，实际项目中可能需要更复杂的逻辑
        if (strcmp(field_name, "_M_impl") == 0 || strcmp(field_name, "_M_start") == 0 || strcmp(field_name, "ptr") == 0) {
            if (!*ptr_field) {
                *ptr_field = field;
            }
        } else if (strcmp(field_name, "_M_finish") == 0 || strcmp(field_name, "size") == 0) {
            if (!*size_field) {
                *size_field = field;
            }
        } else if (strcmp(field_name, "_M_end_of_storage") == 0 || strcmp(field_name, "capacity") == 0) {
            if (!*capacity_field) {
                *capacity_field = field;
            }
        }
    }
}

// 分析指针来源类型
static DataSourceType analyze_pointer_source(tree expr) {
    if (!expr) return DATA_SOURCE_UNKNOWN;
    
    switch (TREE_CODE(expr)) {
        case CALL_EXPR: {
            // 检查是否是函数返回值
            tree callee = CALL_EXPR_FN(expr);
            if (callee && TREE_CODE(callee) == ADDR_EXPR) {
                callee = TREE_OPERAND(callee, 0);
                if (callee && TREE_CODE(callee) == FUNCTION_DECL) {
                    const char *fn_name = IDENTIFIER_POINTER(DECL_NAME(callee));
                    // 常见的内存分配函数
                    if (strcmp(fn_name, "malloc") == 0 || strcmp(fn_name, "calloc") == 0 ||
                        strcmp(fn_name, "realloc") == 0 || strcmp(fn_name, "operator new[]") == 0) {
                        return DATA_SOURCE_DIRECT_FROM_RETURN;
                    }
                }
            }
            break;
        }
        case NULL_EXPR:
            return DATA_SOURCE_NULLPTR;
        case INTEGER_CST:
        case STRING_CST:
        case REAL_CST:
            return DATA_SOURCE_LITERAL;
        default:
            return DATA_SOURCE_COMPLEX;
    }
    
    return DATA_SOURCE_COMPLEX;
}

// 跟踪指针写入操作
static void track_pointer_writes(gimple *stmt, VibeBusinessContext *ctx) {
    if (!stmt || !ctx) return;
    
    if (is_gimple_assign(stmt)) {
        tree lhs = gimple_assign_lhs(stmt);
        tree rhs = gimple_assign_rhs1(stmt);
        
        // 检查是否是指针类型
        if (POINTER_TYPE_P(TREE_TYPE(lhs))) {
            DataSourceType source_type = analyze_pointer_source(rhs);
            
            // 更新统计信息
            ctx->pointer_stats.total_count++;
            switch (source_type) {
                case DATA_SOURCE_DIRECT_FROM_RETURN:
                    ctx->pointer_stats.direct_from_return_count++;
                    break;
                case DATA_SOURCE_NULLPTR:
                    ctx->pointer_stats.nullptr_count++;
                    break;
                case DATA_SOURCE_LITERAL:
                    ctx->pointer_stats.literal_count++;
                    break;
                case DATA_SOURCE_COMPLEX:
                    ctx->pointer_stats.complex_count++;
                    break;
                default:
                    break;
            }
        }
    }
}

// 收集字段数据来源
static void collect_field_data_sources(function *fn, VibeBusinessContext *ctx) {
    if (!fn || !ctx) return;
    
    basic_block bb;
    gimple_stmt_iterator gsi;
    
    // 遍历所有基本块
    FOR_EACH_BB_FN(bb, fn) {
        // 遍历所有语句
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            track_pointer_writes(stmt, ctx);
        }
    }
}

// 处理模板实例化
static void handle_template_instantiation_impl(tree t, VibeBusinessContext *ctx) {
    if (!t || !ctx || !ctx->initialized) return;

    // 检查是否是函数类型
    if (TREE_CODE(t) == FUNCTION_DECL) {
        function *fn = DECL_STRUCT_FUNCTION(t);
        if (!fn) return;

        // 获取函数名
        const char *fn_name = IDENTIFIER_POINTER(DECL_NAME(t));

        // 检查是否是容器相关函数
        if (strstr(fn_name, "vector") != nullptr || strstr(fn_name, "push_back") != nullptr ||
            strstr(fn_name, "emplace_back") != nullptr || strstr(fn_name, "insert") != nullptr) {

            CUTIE_DEBUG_PRINT_RAW(stderr, "Processing template function: %s", fn_name);
            collect_field_data_sources(fn, ctx);
        }
    }
}

// 初始化业务上下文
static error_code_t initialize_business_context(VibeBusinessContext *ctx, CutieContext *cutie_ctx, error_code_t *result) {
    if (!ctx || !cutie_ctx) {
        if (result) *result = ERROR_RECOVERABLE;
        return ERROR_RECOVERABLE;
    }
    
    ctx->cutie_context = cutie_ctx;
    ctx->initialized = true;
    
    // 初始化统计数据
    memset(&ctx->pointer_stats, 0, sizeof(PointerSourceStats));
    
    CUTIE_DEBUG_PRINT_RAW(stderr, "Business context initialized");
    
    if (result) *result = ERROR_OK;
    return ERROR_OK;
}

// 清理业务上下文
static void cleanup_business_context(VibeBusinessContext *ctx) {
    if (!ctx) return;
    
    // 打印统计信息
    if (ctx->initialized && ctx->cutie_context) {
        FILE *debug_file = ctx->cutie_context->debug_file ? ctx->cutie_context->debug_file : stderr;
        
        CUTIE_DEBUG_PRINT_RAW(debug_file, "Pointer source statistics:\n"
                             "  Direct from return: %lld\n"
                             "  nullptr: %lld\n"
                             "  Literal: %lld\n"
                             "  Complex: %lld\n"
                             "  Total: %lld",
                             ctx->pointer_stats.direct_from_return_count,
                             ctx->pointer_stats.nullptr_count,
                             ctx->pointer_stats.literal_count,
                             ctx->pointer_stats.complex_count,
                             ctx->pointer_stats.total_count);
    }
    
    ctx->initialized = false;
    ctx->cutie_context = NULL;
    memset(&ctx->pointer_stats, 0, sizeof(PointerSourceStats));
}

// 注册回调函数实现
static void register_callbacks_impl(plugin_name_args *plugin_info, void *user_data) {
    // 只在LTO链接阶段注册回调
    if (g_is_lto_link_phase) {
        // 注册模板实例化回调
        register_callback(plugin_info->base_name, PLUGIN_FINISH_TYPE, handle_template_instantiation, NULL);
        register_callback(plugin_info->base_name, PLUGIN_FINISH, plugin_finish, NULL);
    }
}

// 处理模板实例化回调函数
extern "C" void handle_template_instantiation(void *gcc_data, void *user_data) {
    // 只在LTO链接阶段处理模板实例化
    if (g_is_lto_link_phase) {
        tree t = (tree)gcc_data;
        handle_template_instantiation_impl(t, &g_business_context);
    }
}

// 插件初始化函数
extern "C" int plugin_init(plugin_name_args *plugin_info, plugin_gcc_version *version) {
    // 检查GCC版本兼容性
    const char *basever = version->basever;
    if (strlen(basever) < 2 || basever[0] != '1' || (basever[1] != '2' && basever[1] != '4')) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE, 0,
                               "Unsupported GCC version: %s (expected 12.x or 14.x)", basever);
        return 1;
    }
    
    // 初始化全局上下文
    memset(&g_cutie_context, 0, sizeof(CutieContext));
    g_cutie_context.debug_file = stderr;
    
    // 使用GCC框架的LTO检测变量
    g_is_lto_mode = (flag_lto != 0);
    
    // 为了兼容性，我们暂时将LTO模式和链接阶段标志设为相同
    // 在实际应用中，可以根据特定GCC版本的API进行更精确的检测
    g_is_lto_link_phase = g_is_lto_mode;
    
    // 输出当前阶段信息用于调试
    if (!g_is_lto_mode) {
        CUTIE_DEBUG_PRINT_RAW(stderr, "Plugin running in non-LTO mode, skipping main functionality");
        return 0;
    }
    
    CUTIE_DEBUG_PRINT_RAW(stderr, "Plugin running in LTO mode");
    
    // 初始化业务上下文
    error_code_t result;
    if (initialize_business_context(&g_business_context, &g_cutie_context, &result) != ERROR_OK) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE, 0,
                               "Failed to initialize business context, result: %lld", result);
        return 1;
    }
    
    CUTIE_DEBUG_PRINT_RAW(stderr, "Plugin initialized in LTO mode");
    
    // 注册回调函数
    register_callbacks_impl(plugin_info, NULL);
    
    return 0;
}

// 注册回调函数
extern "C" void register_callbacks(plugin_name_args *plugin_info, plugin_gcc_version *version) {
    register_callbacks_impl(plugin_info, NULL);
}

// 插件结束函数
extern "C" void plugin_finish(void *gcc_data, void *user_data) {
    cleanup_business_context(&g_business_context);
    CUTIE_DEBUG_PRINT_RAW(stderr, "Plugin finished");
}
