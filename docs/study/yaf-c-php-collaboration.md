# Yaf C/PHP 协作模式 — 学习笔记

> 来源：https://github.com/laruence/yaf
> 作者：Xinchen Hui (laruence@php.net)
> 版本：3.3.9-dev
> 学习日期：2026-08-26

---

## 一、解决什么问题

Yaf 是一个 C 写的 PHP 框架。核心矛盾：**框架的骨架（路由、分发、类加载）需要极致性能（C），但业务逻辑（控制器、模型、插件）需要灵活性（PHP）**。

这与 beacon 的处境高度相似：beacon 的 C 层做感知和调度（性能关键），PHP 层做注册中心通信和健康检查（灵活性关键）。

## 二、文件组织

### 2.1 仓库结构

```
yaf/
├── php_yaf.h                  # 中枢头文件（模块全局 + 标志位 + 宏）
├── yaf.c                      # 模块入口（MINIT/RINIT/RSHUTDOWN）
│
├── yaf_application.c/.h       # Application 类（单例，生命周期）
├── yaf_dispatcher.c/.h        # Dispatcher 类（分发管线，插件调度）
├── yaf_controller.c/.h        # Controller 抽象类
├── yaf_action.c/.h            # Action 抽象类
├── yaf_bootstrap.c/.h         # Bootstrap 抽象类
├── yaf_config.c/.h            # Config 类（INI/PHP 数组解析）
├── yaf_loader.c/.h            # Loader 类（自动加载）
├── yaf_registry.c/.h          # Registry 类（静态 KV 存储）
├── yaf_request.c/.h           # Request 抽象类
├── yaf_response.c/.h          # Response 抽象类
├── yaf_router.c/.h            # Router 类
├── yaf_session.c/.h           # Session 类
├── yaf_view.c/.h              # View 类
├── yaf_plugin.c/.h            # Plugin 抽象类
├── yaf_exception.c/.h         # 异常类层次
│
├── yaf_*.stub.php             # PHP 桩文件（生成 arginfo）
├── yaf_*_arginfo.h            # 生成的参数信息头文件
│
├── configs/                   # 配置解析子模块
├── requests/                  # 请求类型子模块（HTTP/CLI/Simple）
├── responses/                 # 响应类型子模块（HTTP/CLI）
├── routes/                    # 路由类型子模块（6 种路由）
├── views/                     # 视图引擎子模块
├── tests/                     # 测试
└── tools/                     # 代码生成器
```

**组织原则**：
- **一个类一个 C 文件**：`yaf_application.c` 只管 Application，`yaf_dispatcher.c` 只管 Dispatcher
- **每个类配套 4 个文件**：`.c`（实现）+ `.h`（声明）+ `.stub.php`（PHP 桩）+ `_arginfo.h`（生成的参数信息）
- **子目录按功能域**：`routes/` 放 6 种路由实现，`requests/` 放 3 种请求类型

### 2.2 与 beacon 的对比

| 维度 | Yaf | beacon |
|------|-----|--------|
| **文件组织** | 平铺（一个类一个文件） | 平铺（一个功能域一个文件） |
| **类数量** | ~15 个类 | 2 个类（Beacon + Beacon\Governance） |
| **子目录** | 有（routes/requests/responses/views/configs） | 无（所有 C 文件在根目录） |
| **stub.php** | 每个类一个 | 无（手写 arginfo） |
| **PHP 文件** | 仅 `yaf.php`（骨架测试脚本，非框架代码） | `governance.php`（治理 worker 入口脚本） |

**beacon 的启示**：beacon 的平铺结构与 Yaf 一致。beacon 的 `governance.php` 是**功能性 PHP 脚本**（治理 worker 入口），而 Yaf 的 `yaf.php` 只是骨架测试残留——这是关键区别。

## 三、C/PHP 协作架构

### 3.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│  PHP Userland（业务代码）                                    │
│  ├─ Bootstrap.php（_init* 方法，C 层自动调用）               │
│  ├─ controllers/Index.php（继承 Yaf_Controller_Abstract）    │
│  ├─ models/*.php（纯 PHP，C 层不感知）                       │
│  ├─ plugins/*.php（继承 Yaf_Plugin_Abstract）                │
│  └─ views/*.phtml（PHP 模板，C 层渲染引擎加载）              │
├─────────────────────────────────────────────────────────────┤
│  C 扩展层（框架骨架）                                        │
│  ├─ yaf_application.c  — 生命周期管理（单例）                │
│  ├─ yaf_dispatcher.c   — 分发管线（路由→控制器→动作→视图）   │
│  ├─ yaf_loader.c       — 类加载（C 层自动加载 PHP 文件）     │
│  ├─ yaf_router.c       — 路由匹配（C 层正则/模式匹配）       │
│  ├─ yaf_config.c       — 配置解析（C 层解析 INI/PHP 数组）   │
│  ├─ yaf_view.c         — 视图渲染（C 层模板引擎）            │
│  └─ yaf_plugin.c       — 插件基类（7 个钩子，空实现）        │
├─────────────────────────────────────────────────────────────┤
│  Zend Engine                                                 │
│  └─ zend_call_function / zend_call_method（C→PHP 桥接）      │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 C/PHP 边界划分

| 层 | 谁做 | 为什么 |
|---|------|--------|
| **路由匹配** | C | 性能关键（每请求一次），正则/模式匹配在 C 层做 |
| **类加载** | C | 性能关键（每请求多次），文件路径解析在 C 层做 |
| **配置解析** | C | 性能关键（启动时一次），INI 解析在 C 层做 |
| **分发管线** | C | 性能关键（每请求一次），dispatch 循环在 C 层做 |
| **视图渲染** | C | 性能关键（每请求一次），模板引擎在 C 层做 |
| **业务逻辑** | PHP | 灵活性关键（控制器/模型/插件），用户代码在 PHP 层 |
| **Bootstrap** | PHP | 灵活性关键（初始化逻辑），`_init*` 方法在 PHP 层 |
| **插件钩子** | PHP | 灵活性关键（7 个钩子），用户继承 `Yaf_Plugin_Abstract` |

**核心原则**：**性能关键路径在 C，灵活性关键路径在 PHP**。C 层是骨架，PHP 层是血肉。

## 四、C→PHP 调用机制

### 4.1 插件钩子（YAF_PLUGIN_HANDLE）

Yaf 的插件系统是 C→PHP 调用的典型范例。7 个钩子在分发管线的不同节点触发：

```c
// yaf_dispatcher.c 中的宏（简化）
#define YAF_PLUGIN_HANDLE(dispatcher, hook) do { \
    if (dispatcher->plugins) { \
        ZEND_HASH_FOREACH_VAL(dispatcher->plugins, plugin) { \
            if (zend_hash_str_exists(&Z_OBJCE_P(plugin)->function_table, hook)) { \
                zval ret; \
                zend_call_method_with_2_params(plugin, Z_OBJCE_P(plugin), \
                    NULL, hook, &ret, request, response); \
                if (!Z_ISUNDEF(ret)) { \
                    zval_ptr_dtor(&ret); \
                } \
            } \
        } ZEND_HASH_FOREACH_END(); \
    } \
} while(0)
```

**分发管线中的钩子触发点**：

```
Yaf_Dispatcher::dispatch()
  │
  ├─ YAF_PLUGIN_HANDLE(routerStartup)        ← 路由开始前
  ├─ yaf_router_route()                      ← C 层路由匹配
  ├─ YAF_PLUGIN_HANDLE(routerShutdown)       ← 路由结束后
  │
  ├─ YAF_PLUGIN_HANDLE(dispatchLoopStartup)  ← 分发循环开始
  │
  ├─ do {
  │     YAF_PLUGIN_HANDLE(preDispatch)       ← 每次分发前
  │     yaf_dispatcher_handle()              ← C 层执行控制器/动作
  │     YAF_PLUGIN_HANDLE(postDispatch)      ← 每次分发后
  │  } while (!dispatched && --nesting > 0)
  │
  ├─ YAF_PLUGIN_HANDLE(dispatchLoopShutdown) ← 循环结束
  └─ YAF_PLUGIN_HANDLE(preResponse)          ← 响应发送前
```

**关键设计**：
- **存在性检查**：`zend_hash_str_exists` 先检查方法是否存在，避免对未覆写的钩子产生空调用开销
- **返回值处理**：返回 `false` 中断流程，返回 `true`/其他继续
- **参数传递**：`zend_call_method_with_2_params` 传入 `$request` 和 `$response`

### 4.2 Bootstrap 自动调用

```c
// yaf_application.c — bootstrap() 方法
PHP_METHOD(yaf_application, bootstrap) {
    // 1. C 层加载 Bootstrap.php
    yaf_loader_import(bootstrap_path, bootstrap_path_len);

    // 2. C 层找到 Bootstrap 类
    ce = zend_hash_find_ptr(EG(class_table), "bootstrap");

    // 3. C 层实例化 Bootstrap 对象
    object_init_ex(&bootstrap, ce);

    // 4. C 层遍历方法表，找到所有 _init* 方法
    ZEND_HASH_FOREACH_STR_KEY_PTR(&(ce->function_table), func, fptr) {
        if (strncmp(ZSTR_VAL(func), "_init", 5) == 0) {
            // 5. C 层调用 PHP 方法
            yaf_call_user_method_with_1_arguments(obj, fptr, dispatcher, &ret);
        }
    } ZEND_HASH_FOREACH_END();
}
```

**关键设计**：
- C 层**遍历 PHP 类的方法表**，找到所有 `_init*` 前缀的方法
- 按定义顺序自动调用，每个方法接收 `Yaf_Dispatcher` 实例作为参数
- 用户只需在 Bootstrap.php 中定义 `_init*` 方法，C 层自动发现和调用

### 4.3 控制器动作执行

```c
// yaf_dispatcher.c — yaf_dispatcher_handle()
static ZEND_HOT int yaf_dispatcher_handle(yaf_dispatcher_object *dispatcher) {
    // 1. C 层解析路由，得到 module/controller/action
    // 2. C 层加载控制器类文件
    ce = yaf_dispatcher_get_controller(app->directory, request, is_def_module);

    // 3. C 层实例化控制器对象
    object_init_ex(&controller, ce);

    // 4. C 层找到动作方法（如 indexAction）
    fptr = zend_hash_str_find_ptr(&(ce->function_table), func_name, func_len);

    // 5. C 层调用 PHP 方法
    yaf_controller_execute(&controller, fptr, count, args, &ret);

    // 6. C 层自动渲染视图
    yaf_controller_render(&controller, current_action, NULL, &res);
}
```

**关键设计**：
- C 层负责**找到**正确的类和方法（路由→类名→文件→类→方法）
- C 层负责**调用** PHP 方法（`zend_call_method`）
- C 层负责**渲染**视图（模板引擎在 C 层）
- PHP 层只负责**业务逻辑**（控制器方法内的代码）

## 五、异常处理

### 5.1 双模式错误处理

```c
// yaf_exception.c
ZEND_COLD void yaf_trigger_error(int type, char *format, ...) {
    if (yaf_is_throw_exception()) {
        // 模式 1：抛异常（Yaf_Exception 层次）
        yaf_throw_exception(type, buf);
    } else {
        // 模式 2：存错误信息 + php_error_docref
        app->err_no = type;
        app->err_msg = msg;
        php_error_docref(NULL, E_RECOVERABLE_ERROR, "%s", msg);
    }
}
```

**双模式**：`throwException(true)` 时抛异常，`catchException(true)` 时捕获并存到 request。

### 5.2 异常层次

```
Exception
└── Yaf_Exception
    ├── Yaf_Exception_StartupError      — 启动失败
    ├── Yaf_Exception_RouterFailed      — 路由失败
    ├── Yaf_Exception_DispatchFailed    — 分发失败
    ├── Yaf_Exception_LoadFailed        — 加载失败
    │   ├── Yaf_Exception_LoadFailed_Module
    │   ├── Yaf_Exception_LoadFailed_Controller
    │   ├── Yaf_Exception_LoadFailed_Action
    │   └── Yaf_Exception_LoadFailed_View
    └── Yaf_Exception_TypeError         — 类型错误
```

### 5.3 分发循环中的异常捕获

```c
// yaf_dispatcher.c — YAF_EXCEPTION_HANDLE 宏
#define YAF_EXCEPTION_HANDLE(dispatcher) \
    if (EG(exception) && yaf_is_catch_exception()) { \
        yaf_dispatcher_exception_handler(dispatcher); \
    }
```

**异常处理流程**：
1. 分发循环中 PHP 代码抛异常
2. C 层捕获异常，设置 `EG(exception) = NULL`
3. 重定向到 Error 控制器的 errorAction
4. 异常对象存入 request 的 `exception` 参数
5. 执行 Error 控制器（PHP 用户代码）

## 六、与 beacon 的对比

### 6.1 C/PHP 边界对比

| 维度 | Yaf | beacon |
|------|-----|--------|
| **C 层职责** | 框架骨架（路由/分发/加载/渲染） | 基础设施（shm/自计数/健康计算/进程管理） |
| **PHP 层职责** | 业务逻辑（控制器/模型/插件） | 治理逻辑（注册中心通信/健康检查/服务发现） |
| **C→PHP 调用** | `zend_call_method`（插件钩子/Bootstrap/控制器） | `zend_call_function`（治理回调 on_keepalive/on_discover） |
| **PHP→C 调用** | PHP 继承 C 抽象类（Yaf_Controller_Abstract） | PHP 调 C API（Beacon::pick() / Beacon\Governance::storeNodes()） |
| **异常处理** | 抛异常 + 错误控制器 | 返回 false/null + php_error_docref |
| **配置** | C 层解析 INI/PHP 数组 | C 层解析 INI（PHP_INI_SYSTEM） |

### 6.2 关键相似点

1. **性能关键路径在 C，灵活性关键路径在 PHP**——两个项目都遵循这一原则
2. **C 层调 PHP 用 `zend_call_function`/`zend_call_method`**——桥接机制一致
3. **PHP 层通过继承/回调注入业务逻辑**——Yaf 用继承（Yaf_Plugin_Abstract），beacon 用回调（setOpt）
4. **C 层做类加载/文件包含**——Yaf 的 `yaf_loader_import()` 和 beacon 的 `governance.php` 都是 C 层加载 PHP 文件

### 6.3 关键差异

| 维度 | Yaf | beacon |
|------|-----|--------|
| **C→PHP 方向** | C 调 PHP 方法（控制器/插件/Bootstrap） | C 调 PHP 回调（on_keepalive/on_discover） |
| **PHP→C 方向** | PHP 继承 C 类（Yaf_Controller_Abstract） | PHP 调 C 静态方法（Beacon::pick()） |
| **调用频率** | 每请求多次（路由/分发/插件/渲染） | 每请求 0-1 次（pick() 按需）+ 治理 worker 定时器 |
| **异常** | 抛异常（Yaf_Exception 层次） | 返回 false/null（PHP 扩展惯例） |
| **PHP 文件** | 用户业务代码（控制器/模型/插件） | 治理脚本（governance.php，内置 FileRegistry） |

### 6.4 beacon 可借鉴的点

**1. 插件钩子的存在性检查**

Yaf 在调 PHP 方法前先检查方法是否存在：

```c
if (zend_hash_str_exists(&Z_OBJCE_P(plugin)->function_table, hook)) {
    zend_call_method_with_2_params(plugin, ...);
}
```

beacon 的 `beacon_callback_invoke()` 可以借鉴——当前 beacon 直接调 `zend_call_function`，如果回调未设置会失败。可以先检查回调是否已设置：

```c
// beacon_callback.c
zval *cb = beacon_callback_get(type);
if (cb && Z_TYPE_P(cb) == IS_CALLABLE) {
    zend_call_function(&fci, &fci_cache);
}
```

**2. Bootstrap 自动发现模式**

Yaf 的 Bootstrap 自动发现 `_init*` 方法并调用，beacon 的治理脚本可以借鉴——治理脚本中定义 `on_keepalive`/`on_discover` 等函数，C 层自动发现和注册。当前 beacon 用 `setOpt()` 手动注册，可以考虑自动发现模式作为补充。

**3. 异常层次设计**

Yaf 的异常层次（StartupError/RouterFailed/DispatchFailed/LoadFailed/TypeError）按错误类型分类。beacon 当前不抛异常（返回 false/null），但如果未来需要更精细的错误分类，可以借鉴 Yaf 的异常层次。

**4. 文件组织**

Yaf 的"一个类一个 C 文件 + 配套 .h + .stub.php + _arginfo.h"模式，beacon 已遵循（一个功能域一个 C 文件）。beacon 可以考虑引入 stub.php 生成 arginfo，减少手写 arginfo 的工作量。

## 七、一句话总结

> **Yaf 的 C/PHP 协作 = C 做框架骨架（路由/分发/加载/渲染），PHP 做业务逻辑（控制器/模型/插件），C→PHP 通过 `zend_call_method` 桥接，PHP→C 通过继承 C 抽象类。性能关键路径在 C，灵活性关键路径在 PHP。**
