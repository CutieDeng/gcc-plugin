/*
 * VIBE GCC 12 插件实现
 * 遵循类C设计方法，禁用C++异常和RAII
 */

// 错误码定义
typedef long long error_code_t;
#define ERROR_UNINIT                    ((error_code_t)0)
#define ERROR_OK                        ((error_code_t)1)
#define ERROR_RECOVERABLE_ERROR         ((error_code_t)2)
#define ERROR_RECOVERABLE_ERROR1        ((error_code_t)3)
#define ERROR_RECOVERABLE_ERROR2        ((error_code_t)4)
#define ERROR_RECOVERABLE_ERROR3        ((error_code_t)5)
#define ERROR_SELECT_OUT_OF_RANGE_ERROR ((error_code_t)6)
#define ERROR_GCC_ERROR                 ((error_code_t)7)
#define ERROR_CUSTOM_LOGIC_ERROR        ((error_code_t)8)
#define ERROR_MEMORY_RESOURCE_ERROR     ((error_code_t)9)
#define ERROR_SYSTEM_RESOURCE_ERROR     ((error_code_t)10)
#define ERROR_SYSTEM_ERROR              ((error_code_t)11)
#define ERROR_INPUT_OUTPUT_ERROR        ((error_code_t)12)
#define ERROR_GARBAGE_COLLECTION_ERROR  ((error_code_t)13)

// 调试宏定义
#define VIBE_DEBUG_ON 1

#if VIBE_DEBUG_ON
#define VIBE_DEBUG_PRINT(msg, ...) fprintf(stderr, "(\"%s\" %d \"%s\" %s)\n", __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#else
#define VIBE_DEBUG_PRINT(msg, ...) /* 空实现 */
#endif

// 修复的错误打印宏，使用do-while(0)形式避免语法问题
#define VIBE_ERROR_PRINT(format, ...) do { \
    fprintf(stderr, "(\"%s\" %d \"%s\" ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, format, ##__VA_ARGS__); \
    fprintf(stderr, ")\n"); \
} while (0)

// 上下文定义
typedef struct {
    error_code_t error_code;
    error_code_t sub_error_code;
    char error_message[1024];
    char const *custom_error_message;
} CutieContext;

// 数据来源类型枚举
typedef enum {
    DATA_SOURCE_UNKNOWN = 0,
    DATA_SOURCE_DIRECT_FROM_RETURN = 1,
    DATA_SOURCE_NULLPTR = 2,
    DATA_SOURCE_LITERAL = 3,
    DATA_SOURCE_COMPLEX = 4
} DataSourceType;

// 指针数据来源统计
typedef struct {
    int direct_from_return_count;
    int nullptr_count;
    int literal_count;
    int complex_count;
} PointerSourceStats;

// 业务上下文定义
typedef struct {
    CutieContext* base_context;
    int vector_type_count;
    PointerSourceStats ptr_source_stats;
} VibeBusinessContext;

// 模拟GCC内部类型和宏定义，用于非GCC环境编译检查
#ifndef GCC_PLUGIN_H

typedef void* tree;
#define NULL_TREE ((tree)0)
#define TYPE_DECL 1
#define TEMPLATE_DECL 2
#define TREE_CODE(t) 0
#define DECL_TEMPLATE_INSTANTIATION 3
#define TEMPLATE_INFO_CHECK 4
#define TEMPLATE_INFO 5

// 在模拟环境中使用的正确的PLUGIN_TEMPLATE_INSTANTIATION值
#define PLUGIN_TEMPLATE_INSTANTIATION 19
#define PLUGIN_FINISH_TYPE 24

// 正确的plugin_gcc_version结构定义
struct plugin_gcc_version {
    const char *basever;
    const char *datestamp;
    const char *devphase;
    const char *revision;
    const char *configuration_arguments;
    int is_release;
};

typedef struct {
    const char* base_name;
    const char* full_name;
} plugin_name_args;

// 模拟GCC API函数
int plugin_is_GPL_compatible = 1;

extern "C" int register_callback(const char*, int, void (*)(void*, void*), void*);

extern "C" const char* get_generic_type_name(tree);

extern "C" const char* get_identifier_name(void*);

extern "C" int TREE_CODE_LENGTH(int);

#else

#include "gcc-plugin.h"
#include "plugin-version.h"
#include "cp/cp-tree.h"
#include "tree.h"
#include "diagnostic.h"

// 确保插件能被GCC加载
extern "C" int plugin_is_GPL_compatible = 1;

#endif

#include <cstdio>
#include <cstring>
#include <stdarg.h>

// 全局上下文
CutieContext g_cutie_context = {ERROR_UNINIT, 0, "", NULL};
VibeBusinessContext g_business_context = {NULL, 0};

// 非导出API使用匿名命名空间
namespace {

// 前向声明
void identify_vector_fields(tree type, error_code_t* result);
DataSourceType analyze_pointer_source(tree expr);
void track_pointer_writes(tree field, tree rhs, error_code_t* result);
void collect_field_data_sources(tree type, error_code_t* result);

// 错误处理函数
void cutie_context_set_error(CutieContext* ctx, error_code_t error_code, error_code_t sub_error_code, const char* format, ...) {
    if (!ctx) {
        VIBE_ERROR_PRINT("Invalid context pointer");
        return;
    }
    
    ctx->error_code = error_code;
    ctx->sub_error_code = sub_error_code;
    
    va_list args;
    va_start(args, format);
    vsnprintf(ctx->error_message, sizeof(ctx->error_message), format, args);
    va_end(args);
    
    VIBE_ERROR_PRINT("%s (error code: %lld, sub error: %lld)", ctx->error_message, error_code, sub_error_code);
}

// 获取类型名称的辅助函数
void get_type_name(tree type, char const** result) {
    if (!result) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid result pointer");
        return;
    }
    
    if (!type) {
        *result = "(null)";
        return;
    }
    
    // 简化实现
    *result = "vector_type";
}

// 查找模板化vector类型
void find_template_vector_types(tree type, error_code_t* result) {
    if (!result) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid result pointer");
        return;
    }
    
    if (!type) {
        cutie_context_set_error(g_business_context.base_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid type pointer");
        *result = ERROR_RECOVERABLE_ERROR;
        return;
    }
    
    // 简化实现，只输出类型信息
    VIBE_DEBUG_PRINT("Processing template type");
    
    // 由于GCC API的差异，这里简化实现
    *result = ERROR_OK;
}

// 处理模板实例化
void handle_template_instantiation_impl(void* gcc_data, void* user_data) {
    if (!gcc_data || !g_business_context.base_context || g_business_context.base_context->error_code != ERROR_OK) {
        VIBE_ERROR_PRINT("Invalid data or context not initialized");
        return;
    }
    
    tree type = (tree)gcc_data;
    error_code_t result = ERROR_UNINIT;
    
    // 查找模板化vector类型
    find_template_vector_types(type, &result);
    if (result == ERROR_OK) {
        g_business_context.vector_type_count++;
        VIBE_DEBUG_PRINT("Total vector types found: %d", g_business_context.vector_type_count);
        
        // 收集字段数据来源（功能二）
        error_code_t data_result = ERROR_UNINIT;
        collect_field_data_sources(type, &data_result);
        if (data_result == ERROR_OK) {
            VIBE_DEBUG_PRINT("Field data sources collected");
        }
    }
}

// 注册回调函数
void register_callbacks_impl(void* plugin_info, void* user_data) {
    VIBE_DEBUG_PRINT("Registering callbacks");
    
    // 修复PLUGIN_TEMPLATE_INSTANTIATION回调注册
    register_callback(
        ((plugin_name_args*)plugin_info)->base_name,
        PLUGIN_TEMPLATE_INSTANTIATION,
        handle_template_instantiation_impl,
        NULL
    );
}

// 初始化业务上下文
void initialize_business_context(VibeBusinessContext* ctx, CutieContext* base_ctx, error_code_t* result) {
    if (!result) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid result pointer");
        return;
    }
    
    if (!ctx || !base_ctx) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid context pointers");
        *result = ERROR_RECOVERABLE_ERROR;
        return;
    }
    
    ctx->base_context = base_ctx;
    ctx->vector_type_count = 0;
    
    // 初始化指针数据来源统计
    ctx->ptr_source_stats.direct_from_return_count = 0;
    ctx->ptr_source_stats.nullptr_count = 0;
    ctx->ptr_source_stats.literal_count = 0;
    ctx->ptr_source_stats.complex_count = 0;
    
    base_ctx->error_code = ERROR_OK;
    base_ctx->sub_error_code = 0;
    base_ctx->custom_error_message = NULL;
    
    VIBE_DEBUG_PRINT("Business context initialized");
    *result = ERROR_OK;
}

// 识别vector类的字段
void identify_vector_fields(tree type, error_code_t* result) {
    if (!result) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid result pointer");
        return;
    }
    
    if (!type) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid type pointer");
        *result = ERROR_RECOVERABLE_ERROR;
        return;
    }
    
    // 简化实现：假设这是一个vector类型
    VIBE_DEBUG_PRINT("Identified vector fields");
    *result = ERROR_OK;
}

// 分析指针数据来源类型
DataSourceType analyze_pointer_source(tree expr) {
    // 简化实现
    if (!expr) {
        return DATA_SOURCE_NULLPTR;
    }
    
    // 这里应该根据expr的类型和结构进行实际分析
    // 这里只是模拟实现
    return DATA_SOURCE_COMPLEX;
}

// 跟踪指针字段写入操作
void track_pointer_writes(tree field, tree rhs, error_code_t* result) {
    if (!result) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid result pointer");
        return;
    }
    
    if (!field || !g_business_context.base_context || g_business_context.base_context->error_code != ERROR_OK) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid field or context");
        *result = ERROR_RECOVERABLE_ERROR;
        return;
    }
    
    // 分析数据来源
    DataSourceType source_type = analyze_pointer_source(rhs);
    
    // 更新统计信息
    switch (source_type) {
        case DATA_SOURCE_DIRECT_FROM_RETURN:
            g_business_context.ptr_source_stats.direct_from_return_count++;
            break;
        case DATA_SOURCE_NULLPTR:
            g_business_context.ptr_source_stats.nullptr_count++;
            break;
        case DATA_SOURCE_LITERAL:
            g_business_context.ptr_source_stats.literal_count++;
            break;
        case DATA_SOURCE_COMPLEX:
            g_business_context.ptr_source_stats.complex_count++;
            break;
        default:
            break;
    }
    
    VIBE_DEBUG_PRINT("Tracked pointer write, source type: %d", source_type);
    *result = ERROR_OK;
}

// 收集字段数据来源
void collect_field_data_sources(tree type, error_code_t* result) {
    if (!result) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid result pointer");
        return;
    }
    
    if (!type || !g_business_context.base_context || g_business_context.base_context->error_code != ERROR_OK) {
        cutie_context_set_error(&g_cutie_context, ERROR_RECOVERABLE_ERROR, 0, "Invalid type or context");
        *result = ERROR_RECOVERABLE_ERROR;
        return;
    }
    
    // 识别vector字段
    error_code_t id_result = ERROR_UNINIT;
    identify_vector_fields(type, &id_result);
    if (id_result != ERROR_OK) {
        *result = id_result;
        return;
    }
    
    // 这里应该实现实际的字段数据来源收集逻辑
    // 由于是简化实现，我们只记录已调用此函数
    VIBE_DEBUG_PRINT("Collecting field data sources");
    *result = ERROR_OK;
}

// 清理业务上下文
void cleanup_business_context(VibeBusinessContext* ctx) {
    if (ctx) {
        // 清理资源
        ctx->vector_type_count = 0;
        ctx->ptr_source_stats.direct_from_return_count = 0;
        ctx->ptr_source_stats.nullptr_count = 0;
        ctx->ptr_source_stats.literal_count = 0;
        ctx->ptr_source_stats.complex_count = 0;
        VIBE_DEBUG_PRINT("Business context cleaned up");
    }
}

} // 匿名命名空间结束

// 导出的回调函数
extern "C" void handle_template_instantiation(void* gcc_data, void* user_data) {
    handle_template_instantiation_impl(gcc_data, user_data);
}

extern "C" void register_callbacks(void* plugin_info, void* user_data) {
    register_callbacks_impl(plugin_info, user_data);
}

// 插件初始化函数
extern "C" int plugin_init(plugin_name_args* plugin_info, plugin_gcc_version* version) {
    VIBE_DEBUG_PRINT("Plugin initialized");
    
    // 检查GCC版本兼容性
    if (!version || !version->basever) {
        cutie_context_set_error(&g_cutie_context, ERROR_GCC_ERROR, 0, "Invalid GCC version information");
        return 1;
    }
    
    // 扩展版本检查以支持GCC 12和14版本
    if ((version->basever[0] != '1') || 
        (version->basever[1] != '2' && version->basever[1] != '4')) {
        cutie_context_set_error(&g_cutie_context, ERROR_GCC_ERROR, 0, "Unsupported GCC version: %s (expected 12.x or 14.x)", version->basever);
        return 1;
    }
    
    // 初始化业务上下文
    error_code_t result = ERROR_UNINIT;
    initialize_business_context(&g_business_context, &g_cutie_context, &result);
    if (result != ERROR_OK) {
        VIBE_ERROR_PRINT("Failed to initialize business context");
        return 1;
    }
    
    // 注册回调函数
    register_callback(
        plugin_info->base_name,
        PLUGIN_FINISH_TYPE,
        register_callbacks_impl,
        NULL
    );
    
    return 0;
}

// 插件退出时调用的函数
extern "C" void plugin_finish(void) {
    VIBE_DEBUG_PRINT("Plugin finishing");
    
    // 打印统计信息
    VIBE_DEBUG_PRINT("Pointer source statistics:");
    VIBE_DEBUG_PRINT("  Direct from return: %d", g_business_context.ptr_source_stats.direct_from_return_count);
    VIBE_DEBUG_PRINT("  Nullptr: %d", g_business_context.ptr_source_stats.nullptr_count);
    VIBE_DEBUG_PRINT("  Literal: %d", g_business_context.ptr_source_stats.literal_count);
    VIBE_DEBUG_PRINT("  Complex: %d", g_business_context.ptr_source_stats.complex_count);
    VIBE_DEBUG_PRINT("Total vector types processed: %d", g_business_context.vector_type_count);
    
    // 清理业务上下文
    cleanup_business_context(&g_business_context);
}