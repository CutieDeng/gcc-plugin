// 错误码定义
#define VIBE_ERROR_SUCCESS            0
#define VIBE_ERROR_INVALID_ARGS       1
#define VIBE_ERROR_NO_MEMORY          2
#define VIBE_ERROR_GCC_API_FAILURE    3
#define VIBE_ERROR_TYPE_NOT_RECOGNIZED 4
#define VIBE_ERROR_TEMPLATE_PROCESS   5

// 调试宏定义
#define VIBE_DEBUG_ON 1

#if VIBE_DEBUG_ON
#define VIBE_DEBUG_PRINT(msg, ...) fprintf(stderr, "[VIBE DEBUG] %s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define VIBE_DEBUG_PRINT(msg, ...) /* 空实现 */
#endif

#define VIBE_ERROR_PRINT(msg, ...) fprintf(stderr, "[VIBE ERROR] %s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__)

// 上下文定义
typedef struct {
    int initialized;
    int error_code;
    char error_message[1024];
} CutieContext;

// 业务上下文定义
typedef struct {
    CutieContext* base_context;
    int vector_type_count;
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
int plugin_is_GPL_compatible = 1;

#endif

#include <cstdio>
#include <cstring>
#include <stdarg.h>

// 全局上下文
CutieContext g_cutie_context = {0};
VibeBusinessContext g_business_context = {0};

// 错误处理函数
extern "C" int cutie_context_set_error(CutieContext* ctx, int error_code, const char* format, ...);

// 获取类型名称的辅助函数
extern "C" const char* get_type_name(tree type);

// 查找模板化vector类型
extern "C" int find_template_vector_types(tree type);

// 处理模板实例化
extern "C" void handle_template_instantiation(void* gcc_data, void* user_data);

// 注册回调函数
extern "C" void register_callbacks(void* plugin_info, void* user_data);

// 初始化业务上下文
extern "C" int initialize_business_context(VibeBusinessContext* ctx, CutieContext* base_ctx);

// 清理业务上下文
extern "C" void cleanup_business_context(VibeBusinessContext* ctx);

// 错误处理函数
extern "C" int cutie_context_set_error(CutieContext* ctx, int error_code, const char* format, ...) {
    if (!ctx) {
        VIBE_ERROR_PRINT("Invalid context pointer");
        return VIBE_ERROR_INVALID_ARGS;
    }
    
    ctx->error_code = error_code;
    
    va_list args;
    va_start(args, format);
    vsnprintf(ctx->error_message, sizeof(ctx->error_message), format, args);
    va_end(args);
    
    VIBE_ERROR_PRINT("%s (error code: %d)", ctx->error_message, error_code);
    return error_code;
}

// 获取类型名称的辅助函数
extern "C" const char* get_type_name(tree type) {
    if (!type) {
        return "(null)";
    }
    
    // 简化实现
    return "vector_type";
}

// 查找模板化vector类型
extern "C" int find_template_vector_types(tree type) {
    if (!type) {
        return cutie_context_set_error(g_business_context.base_context, VIBE_ERROR_INVALID_ARGS,
                                     "Invalid type pointer");
        
    }
    
    // 简化实现，只输出类型信息
    VIBE_DEBUG_PRINT("Processing template type");
    
    // 由于GCC API的差异，这里简化实现
    return VIBE_ERROR_SUCCESS;
}

// 处理模板实例化
extern "C" void handle_template_instantiation(void* gcc_data, void* user_data) {
    if (!gcc_data || !g_business_context.base_context || !g_business_context.base_context->initialized) {
        VIBE_ERROR_PRINT("Invalid data or context not initialized");
        return;
    }
    
    tree type = (tree)gcc_data;
    
    // 查找模板化vector类型
    if (find_template_vector_types(type) == VIBE_ERROR_SUCCESS) {
        g_business_context.vector_type_count++;
        VIBE_DEBUG_PRINT("Total vector types found: %d", g_business_context.vector_type_count);
    }
}

// 注册回调函数
extern "C" void register_callbacks(void* plugin_info, void* user_data) {
    VIBE_DEBUG_PRINT("Registering callbacks");
    
    // 修复PLUGIN_TEMPLATE_INSTANTIATION回调注册
    register_callback(
        ((plugin_name_args*)plugin_info)->base_name,
        PLUGIN_TEMPLATE_INSTANTIATION,
        handle_template_instantiation,
        NULL
    );
}

// 初始化业务上下文
extern "C" int initialize_business_context(VibeBusinessContext* ctx, CutieContext* base_ctx) {
    if (!ctx || !base_ctx) {
        return VIBE_ERROR_INVALID_ARGS;
    }
    
    ctx->base_context = base_ctx;
    ctx->vector_type_count = 0;
    base_ctx->initialized = 1;
    
    VIBE_DEBUG_PRINT("Business context initialized");
    return VIBE_ERROR_SUCCESS;
}

// 清理业务上下文
extern "C" void cleanup_business_context(VibeBusinessContext* ctx) {
    if (ctx) {
        // 清理资源
        ctx->vector_type_count = 0;
        VIBE_DEBUG_PRINT("Business context cleaned up");
    }
}

// 插件初始化函数
extern "C" int plugin_init(plugin_name_args* plugin_info, plugin_gcc_version* version) {
    VIBE_DEBUG_PRINT("Plugin initialized");
    
    // 检查GCC版本兼容性
    if (!version || !version->basever) {
        cutie_context_set_error(&g_cutie_context, VIBE_ERROR_GCC_API_FAILURE,
                               "Invalid GCC version information");
        return 1;
    }
    
    // 简单版本检查
    // if (version->basever[0] != '1' || version->basever[1] != '2') {
    //     cutie_context_set_error(&g_cutie_context, VIBE_ERROR_GCC_API_FAILURE,
    //                            "Unsupported GCC version: %s (expected 12.x)",
    //                            version->basever);
    //     return 1;
    // }
    
    // 初始化业务上下文
    if (initialize_business_context(&g_business_context, &g_cutie_context) != VIBE_ERROR_SUCCESS) {
        VIBE_ERROR_PRINT("Failed to initialize business context");
        return 1;
    }
    
    // 注册回调函数
    register_callback(
        plugin_info->base_name,
        PLUGIN_FINISH_TYPE,
        register_callbacks,
        NULL
    );
    
    return 0;
}

// 插件退出时调用的函数
extern "C" void plugin_finish(void) {
    VIBE_DEBUG_PRINT("Plugin finishing");
    // 清理业务上下文
    cleanup_business_context(&g_business_context);
}
