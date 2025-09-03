#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "tree-dump.h"
#include "tree-pass.h"
#include "diagnostic.h"
#include "gimple-expr.h"
#include "gimple.h"
#include "ssa.h"
#include "tree-ssa-operands.h"
#include "tree-phinodes.h"
#include "tree-cfg.h"
#include "tree-inline.h"
#include "tree-ssa-loop-niter.h"
#include "tree-ssa-loop-manip.h"
#include "tree-dfa.h"
#include "tree-scalar-evolution.h"
#include "tree-ssa-loop-ivopts.h"
// #include "tree-ssa-loop-prefetch.h"
// #include "tree-ssa-loop-nested.h"
// #include "tree-ssa-loop-im.h"
// #include "tree-ssa-loop-ivcanon.h"
#include "tree-ssanames.h"
#include "gimple-ssa.h"
#include "tree-ssa-address.h"
#include "tree-chrec.h"
// #include "tree-scalar-evolution-complex.h"
#include "tree-scalar-evolution.h"
#include "tree-data-ref.h"
#include "tree-vectorizer.h"
#include "tree-ssa-loop-ivopts.h"
#include "tree-ssa-loop-manip.h"
#include "tree-ssa-loop-prefetch.h"
#include "tree-ssa-loop-nested.h"
#include "tree-ssa-loop-im.h"
#include "tree-ssa-loop-ivcanon.h"
#include "tree-ssanames.h"
#include "gimple-ssa.h"
#include "tree-ssa-address.h"
#include "tree-chrec.h"
#include "tree-scalar-evolution-complex.h"
#include "tree-scalar-evolution.h"
#include "tree-data-ref.h"
#include "tree-vectorizer.h"
#include "tree-pretty-print.h"
#include "gimple-pretty-print.h"
#include "tree-into-ssa.h"
#include "tree-ssa-copyrename.h"
#include "tree-ssa-dom.h"
#include "tree-ssa-coalesce.h"
#include "tree-ssa-live.h"
#include "tree-ssa-uncprop.h"
#include "tree-ssa-operands.h"
#include "tree-ssa-pre.h"
#include "tree-ssa-sink.h"
#include "tree-ssa-ter.h"
#include "tree-ssa-threadedge.h"
#include "tree-ssa-threadupdate.h"
#include "tree-ssa-loop-ivopts.h"
#include "tree-ssa-loop-manip.h"
#include "tree-ssa-loop-prefetch.h"
#include "tree-ssa-loop-nested.h"
#include "tree-ssa-loop-im.h"
#include "tree-ssa-loop-ivcanon.h"
#include "tree-ssanames.h"
#include "gimple-ssa.h"
#include "tree-ssa-address.h"
#include "tree-chrec.h"
#include "tree-scalar-evolution-complex.h"
#include "tree-scalar-evolution.h"
#include "tree-data-ref.h"
#include "tree-vectorizer.h"
#include "tree-pretty-print.h"
#include "gimple-pretty-print.h"
#include "tree-into-ssa.h"
#include "tree-ssa-copyrename.h"
#include "tree-ssa-dom.h"
#include "tree-ssa-coalesce.h"
#include "tree-ssa-live.h"
#include "tree-ssa-uncprop.h"
#include "tree-ssa-operands.h"
#include "tree-ssa-pre.h"
#include "tree-ssa-sink.h"
#include "tree-ssa-ter.h"
#include "tree-ssa-threadedge.h"
#include "tree-ssa-threadupdate.h"

using err_t = long long;

enum error_type : error_t {
  ErrorOk,
  ErrorWeakAssert,
  ErrorStrongAssert,
  ErrorGccLogic,
  ErrorCustomLogic,
  ErrorMemResource,
  ErrorIo,
  ErrorAnalysisFailed,
  ErrorPatternNotFound
};

#define PATTERN_BEGIN err_t ret = ErrorOk;
#define PATTERN_SAFE_CHECK_STRONG() do { if (ret != ErrorOk) { return ret; } } while (0)
#define PATTERN_SAFE_CHECK_WEAK() do { if (ret == ErrorErrorStrongAssert) { return ret; } } while (0)
#define PATTERN_MATCH(x, y, e, strong) do { \
  if (x == NULL_TREE) { \
    y = NULL_TREE; \
  } else { \
    if (strong && !tree_int_cst_equal(x, y)) { \
      ret = e; \
      PATTERN_SAFE_CHECK_STRONG(); \
    } \
  } \
} while (0)
#define PATTERN_END return ret;
#define DEBUG(fmt, ...) do { \
  fprintf(stderr, "DEBUG: %s:%d:%s(): " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
} while (0)

enum vector_pattern_status {
  SUPPORTED,
  OPPOSED,
  IRRELEVANT
};

struct VectorPatternInfo {
  tree decl;
  tree type;
  location_t location;
  vector_pattern_status status;
  const char* evidence;
  
  VectorPatternInfo() : decl(NULL_TREE), type(NULL_TREE), 
                       location(UNKNOWN_LOCATION), 
                       status(IRRELEVANT), 
                       evidence(nullptr) {}
};

struct FunctionAnalysisContext {
  tree func_decl;
  auto_vector<VectorPatternInfo> patterns;
  
  FunctionAnalysisContext() : func_decl(NULL_TREE) {}
};

class VectorPatternAnalyzer {
private:
  auto_vector<FunctionAnalysisContext> function_contexts;
  auto_vector<VectorPatternInfo> all_patterns;
  bool initialized;
  
public:
  VectorPatternAnalyzer() : initialized(false) {}
  
  err_t init() {
    PATTERN_BEGIN
    if (initialized) {
      DEBUG("Analyzer already initialized");
      ret = ErrorOk;
      PATTERN_END;
    }
    
    DEBUG("Initializing VectorPatternAnalyzer");
    initialized = true;
    ret = ErrorOk;
    PATTERN_END;
  }
  
  err_t analyze_function(tree func) {
    PATTERN_BEGIN
    if (!initialized) {
      DEBUG("Analyzer not initialized");
      ret = ErrorGccLogic;
      PATTERN_END;
    }
    
    if (func == NULL_TREE || TREE_CODE(func) != FUNCTION_DECL) {
      DEBUG("Invalid function provided");
      ret = ErrorGccLogic;
      PATTERN_END;
    }
    
    DEBUG("Analyzing function: %s", DECL_NAME_POINTER(func));
    
    FunctionAnalysisContext context;
    context.func_decl = func;
    
    // 创建分析上下文
    if (!analyze_vector_patterns(context)) {
      ret = ErrorAnalysisFailed;
      PATTERN_END;
    }
    
    function_contexts.safe_push(context);
    all_patterns.safe_concat(context.patterns);
    
    ret = ErrorOk;
    PATTERN_END;
  }
  
  err_t finish() {
    PATTERN_BEGIN
    if (!initialized) {
      DEBUG("Analyzer not initialized");
      ret = ErrorGccLogic;
      PATTERN_END;
    }
    
    DEBUG("Analysis finished, reporting findings");
    
    // 输出所有发现的模式
    for (size_t i = 0; i < all_patterns.length(); ++i) {
      VectorPatternInfo& info = all_patterns[i];
      const char* status_str;
      
      switch (info.status) {
        case SUPPORTED: status_str = "SUPPORTED"; break;
        case OPPOSED: status_str = "OPPOSED"; break;
        case IRRELEVANT: status_str = "IRRELEVANT"; break;
        default: status_str = "UNKNOWN"; break;
      }
      
      fprintf(stderr, "Vector Pattern:\n");
      fprintf(stderr, "  Location: %s:%d\n", 
              LOCATION_FILE(info.location), 
              LOCATION_LINE(info.location));
      fprintf(stderr, "  Status: %s\n", status_str);
      fprintf(stderr, "  Evidence: %s\n", info.evidence ? info.evidence : "None");
      fprintf(stderr, "  Declaration: %s\n", 
              info.decl ? DECL_NAME_POINTER(info.decl) : "None");
      fprintf(stderr, "  Type: %s\n", 
              info.type ? type_as_string(info.type, TFF_PLAIN_IDENTIFIER) : "None");
      fprintf(stderr, "\n");
    }
    
    DEBUG("Total patterns found: %zu", all_patterns.length());
    
    initialized = false;
    ret = ErrorOk;
    PATTERN_END;
  }

private:
  // 分析函数中的vector模式
  bool analyze_vector_patterns(FunctionAnalysisContext& context) {
    PATTERN_BEGIN
    DEBUG("Starting vector pattern analysis for function");
    
    // 遍历函数中的所有语句
    gimple_seq body = gimple_body(context.func_decl);
    if (!body) {
      DEBUG("Function has no body");
      ret = ErrorOk;
      PATTERN_END;
    }
    
    for (gimple_stmt_iterator gsi = gsi_start_seq(body); !gsi_end_p(gsi); gsi_next(&gsi)) {
      gimple stmt = gsi_stmt(gsi);
      
      // 查找可能的vector操作
      if (is_vector_assignment(stmt)) {
        analyze_vector_assignment(context, stmt);
      }
      else if (is_vector_resize(stmt)) {
        analyze_vector_resize(context, stmt);
      }
      else if (is_vector_push_back(stmt)) {
        analyze_vector_push_back(context, stmt);
      }
      else if (is_vector_access(stmt)) {
        analyze_vector_access(context, stmt);
      }
    }
    
    ret = ErrorOk;
    PATTERN_END;
  }
  
  // 检查是否是vector赋值操作
  bool is_vector_assignment(gimple stmt) {
    if (gimple_assign_p(stmt)) {
      tree lhs = gimple_assign_lhs(stmt);
      tree rhs = gimple_assign_rhs1(stmt);
      
      // 检查是否是内存访问
      if (TREE_CODE(lhs) == MEM_REF && TREE_CODE(rhs) == MEM_REF) {
        return true;
      }
    }
    return false;
  }
  
  // 分析vector赋值操作
  void analyze_vector_assignment(FunctionAnalysisContext& context, gimple stmt) {
    PATTERN_BEGIN
    
    tree lhs = gimple_assign_lhs(stmt);
    tree rhs = gimple_assign_rhs1(stmt);
    
    // 检查指针、大小和容量之间的关系
    tree ptr1 = TREE_OPERAND(lhs, 0);
    tree ptr2 = TREE_OPERAND(rhs, 0);
    
    VectorPatternInfo info;
    info.decl = context.func_decl;
    info.location = gimple_location(stmt);
    
    // 如果指针相等，可能是resize操作
    if (operand_equal_p(ptr1, ptr2, 0)) {
      info.status = IRRELEVANT;
      info.evidence = "Pointer assignment to same location, not a vector operation";
    }
    // 如果指针不同，可能是memcpy操作
    else {
      info.status = SUPPORTED;
      info.evidence = "Different pointers involved, suggests vector memory copy operation";
    }
    
    context.patterns.safe_push(info);
    ret = ErrorOk;
    PATTERN_END;
  }
  
  // 检查是否是vector resize操作
  bool is_vector_resize(gimple stmt) {
    if (gimple_call_p(stmt)) {
      tree fn = gimple_call_fndecl(stmt);
      if (fn && DECL_NAME(fn)) {
        const char* name = IDENTIFIER_POINTER(DECL_NAME(fn));
        if (strcmp(name, "resize") == 0 || strcmp(name, "reserve") == 0) {
          return true;
        }
      }
    }
    return false;
  }
  
  // 分析vector resize操作
  void analyze_vector_resize(FunctionAnalysisContext& context, gimple stmt) {
    PATTERN_BEGIN
    
    VectorPatternInfo info;
    info.decl = context.func_decl;
    info.location = gimple_location(stmt);
    
    // 检查是否有capacity相关的参数
    tree arg = gimple_call_arg(stmt, 0);
    if (arg && TREE_CODE(arg) == INTEGER_CST) {
      HOST_WIDE_INT value = TREE_INT_CST_LOW(arg);
      
      if (value > 0) {
        info.status = SUPPORTED;
        info.evidence = "Resize operation with explicit size parameter";
      }
      else {
        info.status = OPPOSED;
        info.evidence = "Invalid resize size parameter";
      }
    }
    else {
      info.status = IRRELEVANT;
      info.evidence = "Resize operation with non-constant size parameter";
    }
    
    context.patterns.safe_push(info);
    ret = ErrorOk;
    PATTERN_END;
  }
  
  // 检查是否是vector push_back操作
  bool is_vector_push_back(gimple stmt) {
    if (gimple_call_p(stmt)) {
      tree fn = gimple_call_fndecl(stmt);
      if (fn && DECL_NAME(fn)) {
        const char* name = IDENTIFIER_POINTER(DECL_NAME(fn));
        if (strcmp(name, "push_back") == 0 || strcmp(name, "push_front") == 0) {
          return true;
        }
      }
    }
    return false;
  }
  
  // 分析vector push_back操作
  void analyze_vector_push_back(FunctionAnalysisContext& context, gimple stmt) {
    PATTERN_BEGIN
    
    VectorPatternInfo info;
    info.decl = context.func_decl;
    info.location = gimple_location(stmt);
    
    // 检查是否有size与capacity的比较
    info.status = SUPPORTED;
    info.evidence = "Push operation suggests vector with dynamic size tracking";
    
    context.patterns.safe_push(info);
    ret = ErrorOk;
    PATTERN_END;
  }
  
  // 检查是否是vector访问操作
  bool is_vector_access(gimple stmt) {
    if (gimple_assign_p(stmt)) {
      tree lhs = gimple_assign_lhs(stmt);
      if (TREE_CODE(lhs) == MEM_REF) {
        return true;
      }
    }
    return false;
  }
  
  // 分析vector访问操作
  void analyze_vector_access(FunctionAnalysisContext& context, gimple stmt) {
    PATTERN_BEGIN
    
    VectorPatternInfo info;
    info.decl = context.func_decl;
    info.location = gimple_location(stmt);
    
    // 检查是否有边界检查
    tree op = TREE_OPERAND(gimple_assign_lhs(stmt), 1);
    if (op && TREE_CODE(op) == INTEGER_CST) {
      HOST_WIDE_INT index = TREE_INT_CST_LOW(op);
      
      if (index >= 0) {
        info.status = SUPPORTED;
        info.evidence = "Valid array access with constant index";
      }
      else {
        info.status = OPPOSED;
        info.evidence = "Invalid array access with negative index";
      }
    }
    else {
      info.status = IRRELEVANT;
      info.evidence = "Array access with non-constant index";
    }
    
    context.patterns.safe_push(info);
    ret = ErrorOk;
    PATTERN_END;
  }
};

static VectorPatternAnalyzer vector_analyzer;

static unsigned int execute_vector_analyzer(void) {
  PATTERN_BEGIN
  
  // 初始化分析器
  if (vector_analyzer.init() != ErrorOk) {
    DEBUG("Failed to initialize analyzer");
    ret = ErrorGccLogic;
    PATTERN_END;
  }
  
  // 遍历所有函数进行分析
  tree func;
  FOR_EACH_FUNCTION(func) {
    if (vector_analyzer.analyze_function(func) != ErrorOk) {
      DEBUG("Failed to analyze function: %s", 
            DECL_NAME_POINTER(func) ? DECL_NAME_POINTER(func) : "(null)");
      ret = ErrorAnalysisFailed;
      PATTERN_END;
    }
  }
  
  // 完成分析并输出结果
  if (vector_analyzer.finish() != ErrorOk) {
    DEBUG("Failed to finish analysis");
    ret = ErrorGccLogic;
    PATTERN_END;
  }
  
  ret = ErrorOk;
  PATTERN_END;
}

namespace {
const pass_data pass_data_vector_analyzer = {
  GIMPLE_PASS,                         /* type */
  "vector_analyzer",                   /* name */
  OPTGROUP_NONE,                       /* optinfo_flags */
  TV_NONE,                             /* tv_id */
  (prop_limit | PROP_ssa),             /* properties_required */
  0,                                   /* properties_provided */
  0,                                   /* properties_destroyed */
  0,                                   /* todo_flags_start */
  0,                                   /* todo_flags_finish */
};

class pass_vector_analyzer : public gimple_optimizing_pass {
public:
  pass_vector_analyzer(gimple_context *ctxt)
    : gimple_optimizing_pass(pass_data_vector_analyzer, ctxt) {}
  
  /* opt_pass methods: */
  virtual unsigned int execute(function *fun) {
    // 设置pass实例编号为1
    ref_pass_instance_number = 1;
    
    // 执行分析
    return execute_vector_analyzer();
  }
}; // class pass_vector_analyzer
} // anon namespace

gimple_optimizing_pass *
make_pass_vector_analyzer(gimple_context *ctxt)
{
  return new pass_vector_analyzer(ctxt);
}
