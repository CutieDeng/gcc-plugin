# vibe code style
## 异常处理
定义 64 位错误码 `using err_t = long long; `
全局异常类定义模式：
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
ErrorOk: 正常状态
ErrorWeakAssert: 弱失败模式，用于模式代码安全回退
ErrorStrongAssert: 强失败模式，表示模式代码失败错误，不应恢复继续运行
ErrorGccLogic: 对 GCC 的算法、数据结构、类型等信息断言失败，导致逻辑错误
ErrorCustomLogic: 其他可能的三方库、自行设计的非模式模块的逻辑错误
ErrorMemResource: 内存资源错误，一般为内存空间不足
ErrorIo: IO 错误
ErrorGc: 垃圾回收过程的断言、逻辑错误（如果设计有垃圾回收机制或框架）

---
禁用 C++ 异常机制
禁用构造器、析构器机制，禁止编写带有 RAII 设计的自定义类型，不建议使用 GCC 自定义类型中带有相关设计的类型
引用传参只允许表达两种语义：返回信息（返回值类型被错误码占用，用引用替代返回功能）；模式验证（输入参数非零值时，表示一次检验；输入参数零值时，表示一次返回；即模式匹配）
## 自定义函数规则：宏模式
定义函数开头宏 `#define PATTERN_BEGIN err_t ret = ErrorOk;` （根据具体模式情形增加内容）
定义安全跳转宏 `#define PATTERN_SAFE_CHECK_LABEL(allow_error, label) do { if (ret != ErrorOk && ret != (allow_error)) { goto fn_final; } } while (0)`
定义快速强跳转宏 `#define PATTERN_SAFE_CHECK_STRONG_LABEL(label) PATTERN_SAFE_CHECK_LABEL (ErrorOk, label)`
定义快速弱跳转宏 `#define PATTERN_SAFE_CHECK_WEAK_LABEL(label) PATTERN_SAFE_CHECK_LABEL (ErrorWeakAssert, label)`
定义快速强返回宏 `#define PATTERN_SAFE_CHECK_STRONG() PATTERN_SAFE_CHECK_STRONG_LABEL(fn_final)`
定义快速弱返回宏 `#define PATTERN_SAFE_CHECK_WEAK() PATTERN_SAFE_CHECK_WEAK_LABEL(fn_final)`
定义模式匹配计算宏 `#define PATTERN_MATCH_RAW(x, y, e) if ((x) == 0) { (x) = (y); } else ((x) != (y)) { ret = (e); }` （在 x 是零值时，将其赋 y 值；否则，执行比较，如果比较不等，则设置错误码 e. 并定义两个额外的宏用于控制是强失败 PATTERN_MATCH_STRONG，还是弱失败 PATTERN_MATCH_WEAK）
定义模式匹配强断言宏 `#define PATTERN_MATCH_STRONG(x, y) PATTERN_MATCH_EAW(x, y, ErrorStrongAssert)`
定义模式匹配弱断言宏 `#define PATTERN_MATCH_STRONG(x, y) PATTERN_MATCH_EAW(x, y, ErrorWeakAssert)`
定义模式执行宏 `#define PATTERN_WRAP (x) do { ret |= (x); } while (0)`
定义模式函数结尾宏 `#define PATTERN_END do { fn_final: return ret; } while (0);`
定义调试信息宏 `#define DEBUG(fmt, ...) ...` 输出调试信息，并附上 filepath:lineno fnname 的代码位置前缀
定义弱异常标志移除宏 `#define PATTERN_STRIP_WEAK() do { ret &= ~ErrorWeakAssert; } while (0)`
以上宏定义均为其内容、语义描述和参考实现，如果需要，请根据实际模块设计、功能对上述宏定义进行增强
根据宏的结构分析，任何语义上的单个操作完成后，`ret` 都可能被写入相应的异常信息，因此需要使用 `PATTERN_SAFE_CHECK_STRONG` 或 `PATTERN_SAFE_CHECK_WEAK` 用于控制和保护当前的控制流继续向下进行。
注意宏的定义结构和其对应的 AST 结构，根据其定义在使用中决定是否增加分号结尾。
例如，_BEGIN, _END 应当是基于类作用域的宏描述，其后不应该有结尾分号；而其余宏都应当支持后增加分号。
## 变量定义
由于 `goto` 在 c++ 使用的特殊要求，应当在函数或作用域的开头，声明所有可能会使用的局部变量信息。
## 语义错误处理
任一错误的标志添加，都应通过位或运算完成，即 `ret |= error_code;`，而不是对 ret 进行裸赋值操作，避免遮蔽原错误标志信息。
## IO 处理
只使用 libc 或低于该抽象层级的输入、输出接口，例如 `printf`.
## 缩进规范
使用标准的 4 indent 空格缩进规范，替代 tab 字符。
## 字符串
只使用 `char const *`, gcc 库中支持的专用字符串支持，或额外自定义的字符串处理机制，禁止使用 `std::string`
## 容器
优先使用 gcc 框架中的 vec, hash 相关类型，不建议使用 c++ STL 库
## 多态类型设计
对于复杂的多态类型设计，应当仿照 rust 模式，通过 union 结构定义多态核心数据段，并额外增加 enum tag 字段，维护多态类型的实际有效类型信息
## 模块设计
定义一个大、非 RAII 类型，称为 Analysis(如果这是一个分析任务)，用于管理整个插件的逻辑流、数据流，定义 `init` 函数，并在合适时间执行一次初始化操作，再开始执行分析或优化逻辑，无需、禁止利用构造函数初始化
## GCC 开发注意事项
gcc pass_info 字段 `ref_pass_instance_number` 应直接赋值为 1, 以说明该 pass 唯一，且没被 clone.
如果插入 ipa pass, 建议在 whole program 后执行，并注意 ipa pass 是如何同时处理各函数且避免在函数初始化前处理它
## 内存分配规范
gcc 框架自身不常直接使用 `new` 操作符进行内存分配，插件开发也应当与 gcc 框架的内存分配方式、管理策略保持一致。
## 输出要求
只输出单个能够编译成 GCC 插件的 c++ 源码文件
## 业务要求
