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

---
禁用 C++ 异常机制
禁用构造器、析构器机制，禁止编写带有 RAII 设计的自定义类型，不建议使用 GCC 自定义类型中带有相关设计的类型
引用传参只允许表达两种语义：返回信息（返回值类型被错误码占用，用引用替代返回功能）；模式验证（输入参数非零值时，表示一次检验；输入参数零值时，表示一次返回；即模式匹配）
## 模式代码
定义模式函数开头宏 `#define PATTERN_BEGIN err_t ret = ErrorOk;` （根据具体模式情形增加内容）
定义模式安全返回宏 `#define PATTERN_SAFE_CHECK_STRONG() do { if (ret != ErrorOk) { return ret; } } while (0)`
定义模式弱安全返回宏，`#define PATTERN_SAFE_CHECK_WEAK() ...` 即使在弱失败断言下仍继续执行，而不是直接返回
定义模式匹配计算宏 `#define PATTERN_MATCH(x, y, e) if ((x) == 0) { (x) = (y); } else ((x) != (y)) { ret = (e); }` （在 x 是零值时，将其赋 y 值；否则，执行比较，如果比较不等，则设置错误码 e. 并定义两个额外的宏用于控制是强失败 PATTERN_MATCH_STRONG，还是弱失败 PATTERN_MATCH_WEAK）
定义模式执行宏 `#define PATTERN_WRAP (x) do { ret |= (x); } while (0)`
定义模式函数结尾宏 `#define PATTERN_END do { return ret; } while (0);`
定义调试信息宏 `#define DEBUG(fmt, ...) ...` 输出调试信息，并附上 filepath:lineno fnname 的代码位置前缀
以上宏定义代码均为简要描述，需要根据实际代码规范和上下文给出完整正确实现。
## IO 处理
只使用 libc 或低于该抽象层级的输入、输出接口，例如 `printf`.
## 缩进规范
使用标准的 4 indent 空格缩进规范，替代 tab 字符。
## 字符串
只使用 `char const *`, gcc 库中支持的专用字符串支持，或额外自定义的字符串处理机制，禁止使用 `std::string`
## 容器
优先使用 gcc 框架中的 vec, hash 相关类型，不建议使用 c++ STL 库
## 模块设计
定义一个大、非 RAII 类型，称为 Analysis(如果这是一个分析任务)，用于管理整个插件的逻辑流、数据流，定义 `init` 函数，并在合适时间执行一次初始化操作，再开始执行分析或优化逻辑，无需、禁止利用构造函数初始化
## GCC 开发注意事项
gcc pass 的字段 `ref_pass_instance_number` 应直接赋值为 1, 这意味着该 pass 唯一，且没有被 clone.
如果插入 ipa pass, 建议在 whole program 后执行，并注意 ipa pass 是如何同时处理各函数且避免在函数初始化前处理它
## 输出要求
只输出单个能够编译成 GCC 插件的 c++ 源码文件
## 业务要求
