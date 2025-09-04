# vibe code style
## Exception Handling
Define 64-bit error code `using err_t = long long; `
Global exception class definition pattern:
```c++
enum error_type : error_t {
  ErrorOk,
  ErrorWeakAssert,
  ErrorStrongAssert,
  ErrorGccLogic,
  ErrorCustomLogic,
  ErrorMemResource,
  ErrorIo,
  ErrorGc,
  ...
}
```
ErrorOk: Normal state
ErrorWeakAssert: Weak failure mode, used for pattern code safe fallback
ErrorStrongAssert: Strong failure mode, indicates a failure error in the pattern code and should not be continued to run
ErrorGccLogic: Assertion failure on GCC's algorithms, data structures, types, etc., leading to logical errors
ErrorCustomLogic: Other possible third-party library or self-designed non-pattern module logic errors
ErrorMemResource: Memory resource error, generally due to insufficient memory space
ErrorIo: IO error
ErrorGc: Garbage collection process assertion and logical errors (if a garbage collection mechanism or framework is designed)

---
Disable C++ exception mechanism
Disable constructor and destructor mechanisms, prohibit writing custom types with RAII design, it is not recommended to use GCC custom types that contain related designs
Reference parameters are only allowed to express two semantics: return information (return value type is occupied by error code, use reference to replace the return function); pattern validation (when input parameter is non-zero value, it indicates a test; when the input parameter is zero value, it indicates a return; that is, pattern matching)
## Custom Function Rules: Macro Pattern
Define the function beginning macro `#define PATTERN_BEGIN err_t ret = ErrorOk;` (add content according to specific pattern scenarios)
Define safe jump macro `#define PATTERN_SAFE_CHECK_LABEL(allow_error, label) do { if (ret != ErrorOk && ret != (allow_error)) { goto fn_final; } } while (0)`
Define fast strong jump macro `#define PATTERN_SAFE_CHECK_STRONG_LABEL(label) PATTERN_SAFE_CHECK_LABEL (ErrorOk, label)`
Define fast weak jump macro `#define PATTERN_SAFE_CHECK_WEAK_LABEL(label) PATTERN_SAFE_CHECK_LABEL (ErrorWeakAssert, label)`
Define fast strong return macro `#define PATTERN_SAFE_CHECK_STRONG() PATTERN_SAFE_CHECK_STRONG_LABEL(fn_final)`
Define fast weak return macro `#define PATTERN_SAFE_CHECK_WEAK() PATTERN_SAFE_CHECK_WEAK_LABEL(fn_final)`
Define pattern matching calculation macro `#define PATTERN_MATCH_RAW(x, y, e) if ((x) == 0) { (x) = (y); } else ((x) != (y)) { ret = (e); }` (when x is zero value, assign y value; otherwise, perform comparison, if the comparison is not equal, set error code e. And define two additional macros for controlling strong failure PATTERN_MATCH_STRONG or weak failure PATTERN_MATCH_WEAK)
Define pattern matching strong assertion macro `#define PATTERN_MATCH_STRONG(x, y) PATTERN_MATCH_EAW(x, y, ErrorStrongAssert)`
Define pattern matching weak assertion macro `#define PATTERN_MATCH_STRONG(x, y) PATTERN_MATCH_EAW(x, y, ErrorWeakAssert)`
Define pattern execution macro `#define PATTERN_WRAP (x) do { ret |= (x); } while (0)`
Define pattern function end macro `#define PATTERN_END do { fn_final: return ret; } while (0);`
Define debug information macro `#define DEBUG(fmt, ...) ...` Output debugging information and append the code location prefix of filepath:lineno fnname
Define weak exception flag removal macro `#define PATTERN_STRIP_WEAK() do { ret &= ~ErrorWeakAssert; } while (0)`
All the above macro definitions are their content, semantic descriptions and reference implementations. If needed, please enhance them according to the actual module design and functionality
Based on the macro structure analysis, after any semantic operation is completed, `ret` may be written with corresponding exception information, so it is necessary to use `PATTERN_SAFE_CHECK_STRONG` or `PATTERN_SAFE_CHECK_WEAK` for controlling and protecting the current control flow to continue downward.
Note the macro definition structure and its corresponding AST structure, decide whether to add a semicolon at the end according to its definition in usage.
For example, _BEGIN and _END should be macro descriptions based on class scope, and there should not be a trailing semicolon after them; while other macros should support adding a semicolon at the end.
## Variable Definition
Due to the special requirements of `goto` in C++, all local variables that may be used should be declared at the beginning of a function or scope.
## Semantic Error Handling
Adding any error flag should be done through bitwise OR operation, that is `ret |= error_code;`, instead of directly assigning to ret, to avoid overwriting original error flag information.
## IO Handling
Only use libc or lower-level input and output interfaces than that abstraction level, for example `printf`.
## Indentation Specification
Use standard 4 indent space indentation specification instead of tab characters.
## Strings
Only use `char const *`, specialized string support in the gcc library, or additional custom string processing mechanisms. Prohibit using `std::string`
## Containers
Prefer using vec, hash related types in the gcc framework. It is not recommended to use c++ STL library.
## Polymorphic Type Design
For complex polymorphic type design, it should be modeled after rust pattern by defining a union structure to define the polymorphic core data segment, and adding an enum tag field to maintain the actual effective type information of the polymorphic type.
## Module Design
Define a large, non-RAII type called Analysis (if this is an analysis task) to manage the entire plugin's logic flow and data flow, define `init` function, and execute initialization operation at appropriate time, then start executing analysis or optimization logic. It is not needed and prohibited to use constructor for initialization.
## GCC Development Notes
gcc pass_info field `ref_pass_instance_number` should be directly assigned as 1 to indicate that the pass is unique and not cloned.
If inserting ipa pass, it is recommended to execute after whole program, and pay attention to how the ipa pass handles each function and avoids processing it before function initialization.
## Memory Allocation Specification
The gcc framework itself rarely uses the `new` operator for memory allocation directly. Plugin development should also be consistent with the memory allocation method and management strategy of the gcc framework.
## Output Requirements
Only output a single C++ source file that can be compiled into a GCC plugin.
## Business Requirements
