/* gcc-racket-serializer.c - GCC plugin that serializes GCC IR to Racket-compatible format */

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <function.h>
#include <basic-block.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <stringpool.h>
#include <tree-pass.h>
#include <context.h>
#include <cgraph.h>
#include <diagnostic.h>

int plugin_is_GPL_compatible;

static const char *plugin_name = "gcc-racket-serializer";
static FILE *output_file = NULL;

/* 错误处理函数 */
static void 
eh_handle(const char *context, const char *reason, void *data)
{
  fprintf(stderr, "WARNING: Unable to process %s: %s\n", context, reason);
  if (data) {
    fprintf(stderr, "  Related data pointer: %p\n", data);
  }
  
  if (output_file) {
    fprintf(output_file, "(unhandled-element \"%s\" \"%s\")", context, reason);
  }
}

/* 字符串引用转义 */
static void
output_escaped_string(FILE *file, const char *str)
{
  if (!str) {
    fprintf(file, "\"\"");
    return;
  }
  
  fputc('"', file);
  for (; *str; str++) {
    switch (*str) {
      case '"': fprintf(file, "\\\""); break;
      case '\\': fprintf(file, "\\\\"); break;
      case '\n': fprintf(file, "\\n"); break;
      case '\r': fprintf(file, "\\r"); break;
      case '\t': fprintf(file, "\\t"); break;
      default:
        if (*str >= 32 && *str <= 126) {
          fputc(*str, file);
        } else {
          fprintf(file, "\\x%02x", (unsigned char)*str);
        }
    }
  }
  fputc('"', file);
}

/* 序列化tree节点 */
static void
serialize_tree(FILE *file, tree t)
{
  if (!t) {
    fprintf(file, "()");
    return;
  }

  enum tree_code code = TREE_CODE(t);
  
  fprintf(file, "((tree-code \"%s\")", get_tree_code_name(code));
  
  switch (code) {
    case INTEGER_CST:
      if (tree_fits_shwi_p(t)) {
        fprintf(file, "(value %ld)", tree_to_shwi(t));
      } else {
        fprintf(file, "(value-too-large #t)");
      }
      break;
      
    case REAL_CST:
      {
        char string[64];
        real_to_decimal(string, TREE_REAL_CST_PTR(t), sizeof(string), 0, 1);
        fprintf(file, "(value %s)", string);
      }
      break;
      
    case STRING_CST:
      fprintf(file, "(value ");
      output_escaped_string(file, TREE_STRING_POINTER(t));
      fprintf(file, ")");
      break;
      
    case IDENTIFIER_NODE:
      fprintf(file, "(identifier ");
      output_escaped_string(file, IDENTIFIER_POINTER(t));
      fprintf(file, ")");
      break;
      
    case VAR_DECL:
    case PARM_DECL:
    case RESULT_DECL:
    case FUNCTION_DECL:
      fprintf(file, "(name ");
      if (DECL_NAME(t)) {
        output_escaped_string(file, IDENTIFIER_POINTER(DECL_NAME(t)));
      } else {
        fprintf(file, "\"<unnamed>\"");
      }
      fprintf(file, ")");
      
      if (code == FUNCTION_DECL) {
        fprintf(file, "(function-decl #t)");
      }
      break;
      
    default:
      /* 更多类型可在此处理 */
      break;
  }
  
  fprintf(file, ")");
}

/* 序列化gimple语句 */
static void
serialize_gimple(FILE *file, gimple *stmt)
{
  if (!stmt) {
    fprintf(file, "()");
    return;
  }
  
  enum gimple_code code = gimple_code(stmt);
  
  fprintf(file, "((gimple-code \"%s\")", gimple_code_name[code]);
  
  /* 处理常见的gimple语句类型 */
  switch (code) {
    case GIMPLE_ASSIGN:
      {
        fprintf(file, "(operation \"%s\")", get_tree_code_name(gimple_assign_rhs_code(stmt)));
        
        fprintf(file, "(lhs ");
        serialize_tree(file, gimple_assign_lhs(stmt));
        fprintf(file, ")");
        
        fprintf(file, "(rhs (");
        for (unsigned i = 0; i < gimple_num_ops(stmt) - 1; i++) {
          serialize_tree(file, gimple_op(stmt, i + 1));
          if (i < gimple_num_ops(stmt) - 2)
            fprintf(file, " ");
        }
        fprintf(file, "))");
      }
      break;
      
    case GIMPLE_CALL:
      {
        tree fn = gimple_call_fn(stmt);
        fprintf(file, "(fn ");
        serialize_tree(file, fn);
        fprintf(file, ")");
        
        if (gimple_call_lhs(stmt)) {
          fprintf(file, "(lhs ");
          serialize_tree(file, gimple_call_lhs(stmt));
          fprintf(file, ")");
        }
        
        fprintf(file, "(args (");
        for (unsigned i = 0; i < gimple_call_num_args(stmt); i++) {
          serialize_tree(file, gimple_call_arg(stmt, i));
          if (i < gimple_call_num_args(stmt) - 1)
            fprintf(file, " ");
        }
        fprintf(file, "))");
      }
      break;
      
    case GIMPLE_COND:
      {
        fprintf(file, "(predicate \"%s\")", get_tree_code_name(gimple_cond_code(stmt)));
        fprintf(file, "(lhs ");
        serialize_tree(file, gimple_cond_lhs(stmt));
        fprintf(file, ")");
        fprintf(file, "(rhs ");
        serialize_tree(file, gimple_cond_rhs(stmt));
        fprintf(file, ")");
        
        /* 我们没有直接访问基本块的索引，这部分会在基本块序列化时处理 */
      }
      break;
      
    case GIMPLE_RETURN:
      {
        greturn *stmt1 = as_a<greturn *> (stmt);
        if (gimple_return_retval(stmt1)) {
          fprintf(file, "(retval ");
          serialize_tree(file, gimple_return_retval(stmt1));
          fprintf(file, ")");
        }
        break;
      }
      
    default:
      /* 对于未详细处理的类型，提供基本信息 */
      fprintf(file, "(num-ops %d)", gimple_num_ops(stmt));
      break;
  }
  
  fprintf(file, ")");
}

/* 序列化gimple序列 */
static void
serialize_gimple_seq(FILE *file, gimple_seq seq)
{
  if (!seq) {
    fprintf(file, "()");
    return;
  }
  
  fprintf(file, "(");
  
  gimple_stmt_iterator gsi;
  bool first = true;
  
  for (gsi = gsi_start(seq); !gsi_end_p(gsi); gsi_next(&gsi)) {
    if (!first) {
      fprintf(file, " ");
    }
    serialize_gimple(file, gsi_stmt(gsi));
    first = false;
  }
  
  fprintf(file, ")");
}

/* 序列化基本块 */
static void
serialize_basic_block(FILE *file, basic_block bb)
{
  if (!bb) {
    fprintf(file, "()");
    return;
  }
  
  fprintf(file, "((");
  
  /* 确定基本块类型 */
  if (bb->index == ENTRY_BLOCK) {
    fprintf(file, "type \"entry\"");
  } else if (bb->index == EXIT_BLOCK) {
    fprintf(file, "type \"exit\"");
  } else {
    fprintf(file, "type \"bb-id\" %d", bb->index);
  }
  
  fprintf(file, ")(gimple-seq ");
  serialize_gimple_seq(file, bb_seq(bb));
  fprintf(file, "))");
}

/* 序列化CFG */
static void
serialize_cfg(FILE *file, struct function *fn)
{
  if (!fn || !fn->cfg) {
    eh_handle("control-flow-graph", "No CFG available", fn);
    fprintf(file, "()");
    return;
  }
  
  fprintf(file, "(");
  
  /* 序列化入口块 */
  serialize_basic_block(file, ENTRY_BLOCK_PTR_FOR_FN(fn));
  
  /* 序列化所有普通基本块 */
  basic_block bb;
  FOR_EACH_BB_FN(bb, fn) {
    fprintf(file, " ");
    serialize_basic_block(file, bb);
  }
  
  /* 序列化出口块 */
  fprintf(file, " ");
  serialize_basic_block(file, EXIT_BLOCK_PTR_FOR_FN(fn));
  
  fprintf(file, ")");
}

/* 序列化函数参数 */
static void
serialize_function_params(FILE *file, tree fndecl)
{
  fprintf(file, "(");
  
  tree arg;
  bool first = true;
  
  for (arg = DECL_ARGUMENTS(fndecl); arg; arg = DECL_CHAIN(arg)) {
    if (!first) {
      fprintf(file, " ");
    }
    fprintf(file, "(");
    
    tree name = DECL_NAME(arg);
    if (name) {
      fprintf(file, "(name ");
      output_escaped_string(file, IDENTIFIER_POINTER(name));
      fprintf(file, ")");
    } else {
      fprintf(file, "(name \"<unnamed>\")");
    }
    
    fprintf(file, "(type ");
    output_escaped_string(file, get_tree_code_name(TREE_CODE(TREE_TYPE(arg))));
    fprintf(file, ")");
    
    fprintf(file, ")");
    first = false;
  }
  
  fprintf(file, ")");
}

/* 序列化函数定义 */
static void
serialize_function_def(FILE *file, tree fndecl)
{
  if (!fndecl || TREE_CODE(fndecl) != FUNCTION_DECL) {
    eh_handle("function-def", "Not a function declaration", fndecl);
    fprintf(file, "()");
    return;
  }
  
  struct function *fn = DECL_STRUCT_FUNCTION(fndecl);
  if (!fn) {
    eh_handle("function-def", "No function structure", fndecl);
    fprintf(file, "()");
    return;
  }
  
  fprintf(file, "((name ");
  output_escaped_string(file, IDENTIFIER_POINTER(DECL_NAME(fndecl)));
  fprintf(file, ")");
  
  fprintf(file, "(params ");
  serialize_function_params(file, fndecl);
  fprintf(file, ")");
  
  fprintf(file, "(other-info ");
  fprintf(file, "(is-static %s)", TREE_PUBLIC(fndecl) ? "#f" : "#t");
  fprintf(file, " (is-inline %s)", DECL_DECLARED_INLINE_P(fndecl) ? "#t" : "#f");
  fprintf(file, ")");
  
  fprintf(file, "(control-flow-graph ");
  serialize_cfg(file, fn);
  fprintf(file, ")");
  
  fprintf(file, ")");
}

/* 序列化整个编译单元 */
static void
serialize_translation_unit(FILE *file)
{
  fprintf(file, "(translate-unit ");
  
  struct cgraph_node *node;
  bool first = true;
  
  /* 遍历所有函数 */
  FOR_EACH_FUNCTION(node) {
    if (node->definition) {
      if (!first) {
        fprintf(file, " ");
      }
      
      serialize_function_def(file, node->decl);
      first = false;
    }
  }
  
  fprintf(file, ")\n");
}

/* 插件执行的主回调 */
static void
racket_serializer_callback(void *gcc_data, void *user_data)
{
  const char *output_path = "gcc-racket-output.rkt";
  
  output_file = fopen(output_path, "w");
  if (!output_file) {
    error("Failed to open output file %s", output_path);
    return;
  }
  
  /* 文件头，确保可以在Racket中直接使用 */
  fprintf(output_file, "#lang racket\n\n");
  fprintf(output_file, ";; GCC Racket Serializer Output\n\n");
  
  serialize_translation_unit(output_file);
  
  fclose(output_file);
  output_file = NULL;
  
  inform(UNKNOWN_LOCATION, "GCC IR serialized to %s", output_path);
}

/* 注册一个简单的pass */
static struct opt_pass racket_serializer_pass = {
  .type = GIMPLE_PASS,
  .name = "racket-serializer",
  .optinfo_flags = OPTGROUP_NONE,
  .tv_id = TV_NONE,
  .properties_required = 0,
  .properties_provided = 0,
  .properties_destroyed = 0,
  .todo_flags_start = 0,
  .todo_flags_finish = 0
};

/* 插件初始化函数 */
int
plugin_init(struct plugin_name_args *plugin_info,
            struct plugin_gcc_version *version)
{
  /* 检查GCC版本兼容性 */
  if (!plugin_default_version_check(version, &gcc_version)) {
    error("This GCC plugin is for version %d%s%d, but is running with %s",
          GCCPLUGIN_VERSION_MAJOR, ".", GCCPLUGIN_VERSION_MINOR,
          version->basever);
    return 1;
  }
  
  /* 注册插件 */
  register_callback(plugin_name,
                   PLUGIN_PASS_MANAGER_SETUP,
                   NULL,
                   &racket_serializer_pass);
  
  /* 注册在编译结束时调用的回调 */
  register_callback(plugin_name,
                   PLUGIN_FINISH_UNIT,
                   racket_serializer_callback,
                   NULL);
  
  return 0;
}
