/* Single-file GCC plugin for std::vector-like container analysis. Compiles with: g++ -shared -o vector_analyzer.so vector_analyzer.cpp -I$(gcc --print-file-name=plugin)/include */

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <tree-pass.h>
#include <cgraph.h>
#include <basic-block.h>
#include <context.h>
#include <diagnostic.h>
#include <intl.h>
#include <plugin.h>
#include <alloc-pool.h>
#include <vec.h>
#include <hash-table.h>
#include <hash-set.h>
#include <hash-map.h>
#include <stdio.h>  /* For printf */
#include <string.h> /* For strlen, etc. */

int plugin_is_GPL_compatible;

/* Error handling */
using err_t = long long;
enum error_type : err_t {
  ErrorOk = 0,
  ErrorWeakAssert,
  ErrorStrongAssert,
  ErrorGccLogic,
  ErrorCustomLogic,
  ErrorMemResource,
  ErrorIo
};

/* Pattern macros */
#define PATTERN_BEGIN err_t ret = ErrorOk;
#define PATTERN_SAFE_CHECK_STRONG() do { if (ret != ErrorOk) { return ret; } } while (0)
#define PATTERN_SAFE_CHECK_WEAK() do { if (ret == ErrorStrongAssert || ret == ErrorGccLogic || ret == ErrorCustomLogic || ret == ErrorMemResource || ret == ErrorIo) { return ret; } } while (0)  /* Continue on weak assert */
#define PATTERN_MATCH(x, y, e) do { if ((x) == 0) { (x) = (y); } else if ((x) != (y)) { ret = (e); } } while (0)
#define PATTERN_MATCH_STRONG(x, y, e) do { PATTERN_MATCH((x), (y), (e)); PATTERN_SAFE_CHECK_STRONG(); } while (0)
#define PATTERN_MATCH_WEAK(x, y, e) do { PATTERN_MATCH((x), (y), (e)); PATTERN_SAFE_CHECK_WEAK(); } while (0)
#define PATTERN_WRAP(x) do { ret |= (x); PATTERN_SAFE_CHECK_STRONG(); } while (0)  /* Assuming |= accumulates errors; strong check */
#define PATTERN_END do { return ret; } while (0)

/* Debug macro (simplified; in real use, integrate with GCC diagnostic) */
#define DEBUG(fmt, ...) do { printf("%s:%d %s: " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); } while (0)

/* Analysis structures */
struct type_info {
  tree type_decl;
  tree ptr_field;
  tree size_field;
  tree cap_field;
};

enum argument_type {
  ArgSupport,
  ArgOppose,
  ArgIrrelevant
};

struct analysis_result {
  tree func_decl;
  type_info* type;
  argument_type arg;
  char const* reason;
};

struct function_decl_hasher : ggc_hasher<tree> {
  static hashval_t hash(tree t) { return DECL_UID(t); }
  static bool equal(tree a, tree b) { return a == b; }
};

struct Analysis {
  VEC(type_info*, heap)* type_candidates;  /* GCC vec */
  hash_table<function_decl_hasher>* func_analysis_map;  /* GCC hash_table */
  err_t last_error;
};

/* Forward declarations */
static err_t analyze_function(Analysis* self, struct cgraph_node* node);
static bool is_vector_like_type(tree type, type_info& info);
static err_t collect_argument(gimple* stmt, type_info* type, argument_type& arg, char const** reason);

/* Init function */
err_t analysis_init(Analysis* self) {
  PATTERN_BEGIN;
  self->type_candidates = VEC_alloc(type_info*, heap, 16);
  PATTERN_MATCH(self->type_candidates, NULL, ErrorGccLogic);  /* Should not be NULL */
  PATTERN_SAFE_CHECK_STRONG();

  allocator* alloc = ggc_allocator();
  self->func_analysis_map = hash_table<function_decl_hasher>::create_ggc(32, alloc);
  PATTERN_MATCH(self->func_analysis_map, NULL, ErrorGccLogic);
  PATTERN_SAFE_CHECK_STRONG();

  self->last_error = ErrorOk;
  PATTERN_END;
}

/* Cleanup function (manual, no RAII) */
err_t analysis_cleanup(Analysis* self) {
  PATTERN_BEGIN;
  if (self->type_candidates) {
    VEC_free(type_info*, heap, self->type_candidates);
  }
  if (self->func_analysis_map) {
    self->func_analysis_map->empty();
    delete self->func_analysis_map;
  }
  self->last_error = ErrorOk;
  PATTERN_END;
}

/* Check if a struct type has vector-like triplet */
bool is_vector_like_type(tree type, type_info& info) {
  if (TREE_CODE(type) != RECORD_TYPE) return false;
  tree fields = TYPE_FIELDS(type);
  int count = 0;
  tree ptr = NULL, size = NULL, cap = NULL;
  for (tree f = fields; f; f = DECL_CHAIN(f)) {
    if (TREE_CODE(f) != FIELD_DECL) continue;
    tree ftype = TREE_TYPE(f);
    if (TREE_CODE(ftype) == POINTER_TYPE && !ptr) ptr = f;
    else if (TREE_CODE(ftype) == INTEGER_TYPE && !size) size = f;
    else if (TREE_CODE(ftype) == INTEGER_TYPE && !cap) cap = f;
    count++;
  }
  if (count == 3 && ptr && size && cap) {
    info.type_decl = type;
    info.ptr_field = ptr;
    info.size_field = size;
    info.cap_field = cap;
    return true;
  }
  return false;
}

/* Collect argument for a statement */
err_t collect_argument(gimple* stmt, type_info* type, argument_type& arg, char const** reason) {
  PATTERN_BEGIN;
  /* Simplified: Check for patterns like realloc on size == cap (support), direct ptr write without size update (oppose), etc. */
  if (gimple_code(stmt) == GIMPLE_ASSIGN) {
    tree lhs = gimple_assign_lhs(stmt);
    tree rhs = gimple_assign_rhs1(stmt);
    if (TREE_CODE(lhs) == COMPONENT_REF && TREE_OPERAND(lhs, 0) == type->ptr_field) {
      if (TREE_CODE(rhs) == CALL_EXPR) {  /* e.g., realloc */
        arg = ArgSupport;
        *reason = "Supports vector: realloc-like on pointer";
      } else {
        arg = ArgOppose;
        *reason = "Opposes vector: direct pointer manipulation without checks";
      }
    } else {
      arg = ArgIrrelevant;
      *reason = "Irrelevant to vector pattern";
    }
  } else {
    arg = ArgIrrelevant;
    *reason = "Irrelevant statement";
  }
  PATTERN_END;
}

/* Analyze a single function for usages */
static err_t analyze_function(Analysis* self, struct cgraph_node* node) {
  PATTERN_BEGIN;
  tree fndecl = node->decl;
  basic_block bb;
  gimple_stmt_iterator gsi;

  FOR_EACH_BB_FN(bb, DECL_STRUCT_FUNCTION(fndecl)) {
    for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
      gimple* stmt = gsi_stmt(gsi);
      for (int i = 0; i < VEC_length(type_info*, self->type_candidates); i++) {
        type_info* ti = VEC_index(type_info*, self->type_candidates, i);
        /* Check if stmt uses ti->type_decl (simplified; in real, check operands) */
        if (gimple_uses_type(stmt, ti->type_decl)) {  /* Custom helper; assume implemented */
          argument_type arg;
          char const* reason = NULL;
          PATTERN_WRAP(collect_argument(stmt, ti, arg, &reason));
          PATTERN_SAFE_CHECK_WEAK();  /* Weak for non-critical */

          analysis_result* res = ggc_alloc<analysis_result>();
          res->func_decl = fndecl;
          res->type = ti;
          res->arg = arg;
          res->reason = reason ? ggc_strdup(reason) : NULL;

          hashval_t h = function_decl_hasher::hash(fndecl);
          analysis_result** slot = self->func_analysis_map->find_slot_with_hash(fndecl, h, INSERT);
          if (!*slot) {
            *slot = VEC_alloc(analysis_result*, heap, 8);
          }
          VEC_safe_push(analysis_result*, heap, *(VEC(analysis_result*, heap)**)slot, res);
        }
      }
    }
  }
  PATTERN_END;
}

/* Run the full analysis */
err_t analysis_run(Analysis* self) {
  PATTERN_BEGIN;

  /* Step 1: Collect candidate types */
  for (tree ns = global_namespace; ns; ns = DECL_CHAIN(ns)) {
    if (TREE_CODE(ns) == TYPE_DECL) {
      tree type = TREE_TYPE(ns);
      type_info info;
      if (is_vector_like_type(type, info)) {
        type_info* ti = ggc_alloc<type_info>();
        *ti = info;
        VEC_safe_push(type_info*, heap, self->type_candidates, ti);
      }
    }
  }
  PATTERN_SAFE_CHECK_STRONG();

  /* Step 2: Analyze functions */
  struct cgraph_node* node;
  FOR_EACH_FUNCTION(node) {
    if (node->has_gimple_body_p()) {
      PATTERN_WRAP(analyze_function(self, node));
      PATTERN_SAFE_CHECK_WEAK();  /* Continue on weak issues */
    }
  }
  PATTERN_SAFE_CHECK_STRONG();

  PATTERN_END;
}

/* Unified output */
err_t analysis_output(Analysis* self) {
  PATTERN_BEGIN;
  hash_table<function_decl_hasher>::iterator iter;
  hashval_t h;
  void* slot;

  FOR_EACH_HASH_TABLE_ELEMENT(*self->func_analysis_map, slot, void*, iter) {
    tree fndecl = (tree)iter.key;  /* Assuming iter provides key */
    VEC(analysis_result*, heap)* results = (VEC(analysis_result*, heap)*)slot;
    char const* fname = IDENTIFIER_POINTER(DECL_NAME(fndecl));

    for (int i = 0; i < VEC_length(analysis_result*, results); i++) {
      analysis_result* res = VEC_index(analysis_result*, results, i);
      char const* tname = IDENTIFIER_POINTER(DECL_NAME(res->type->type_decl));
      char const* arg_str = (res->arg == ArgSupport) ? "support" :
                            (res->arg == ArgOppose) ? "oppose" : "irrelevant";
      printf("Function %s: Type %s - Argument: %s - Reason: %s\n", fname, tname, arg_str, res->reason ? res->reason : "N/A");
    }
  }
  PATTERN_END;
}

/* Pass data */
static unsigned int vector_analyzer_execute(void) {
  Analysis anal;
  err_t ret = analysis_init(&anal);
  if (ret != ErrorOk) {
    DEBUG("Init failed: %lld", ret);
    return 1;
  }

  ret = analysis_run(&anal);
  if (ret != ErrorOk) {
    DEBUG("Run failed: %lld", ret);
  }

  ret = analysis_output(&anal);
  if (ret != ErrorOk) {
    DEBUG("Output failed: %lld", ret);
  }

  analysis_cleanup(&anal);
  return 0;
}

static struct ipa_opt_pass_d pass_vector_analyzer = {
  {
    OPT_PASS_EXECUTE, "vector_analyzer", NULL, NULL, NULL, vector_analyzer_execute, NULL, NULL, NULL,
    0, TV_IPA_OPT, PROP_gimple_any | PROP_cfg, 0, 0, 0, 0, 1  /* ref_pass_instance_number = 1 */
  },
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* Plugin entry */
int plugin_init(struct plugin_name_args* plugin_info, struct plugin_gcc_version* version) {
  if (!plugin_default_version_check(version, &gcc_version)) return 1;

  struct register_pass_info pass_info;
  pass_info.pass = &pass_vector_analyzer.pass;
  pass_info.reference_pass_name = "whole-program";  /* After whole-program */
  pass_info.ref_pass_instance_number = 1;
  pass_info.pos_op = PASS_POS_INSERT_AFTER;

  register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
  return 0;
}

/* Custom helper (stub for compilation; implement fully as needed) */
bool gimple_uses_type(gimple* stmt, tree type) {
  /* Stub: Always return true for demo */
  return true;
}
