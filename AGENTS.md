# 项目约定

本目录是可独立构建的仓库根目录。后续源码维护以本目录为准，不依赖父目录的旧工程、`pcl`、`reports`、个人 SDK 路径或缓存。普通沟通使用中文。

## 必须维持的交付形式

- Release 上传 `winmm.dll`（PE Machine `0x8664`，x64）、`version.dll`（`0x014C`，x86）和 `HKRegion.HMCL.Agent.jar`（Java 17+，与 JVM 位数无关）。普通用户仍只需将两个 DLL 一起放到启动器 EXE 同目录；独立 JAR 供高级用户通过 `-javaagent` 使用。
- 独立 JAR 必须与两个 DLL 内嵌的 agent 字节一致，并包含 `META-INF/LICENSE`。打包及 CI 下载校验需同时覆盖三份产物的大小、SHA-256、DLL 架构和 agent 清单/类；不能把 JAR 交给 PE 校验分支。
- PCL2 / PCL2-CE 使用 x64 WinMM 入口；HMCL 的 x86 壳使用 version 入口，再给实际 JVM 加载与架构无关的 Java agent。不能以 Windows 位数或 JVM 位数替代壳位数选择 DLL。
- 同一架构的 version/winmm/winhttp 副本字节相同，仅供内部转发测试；不同架构不相同。不要把 x64 version.dll 或 x86 winmm.dll 混入正式发布。
- 不修改磁盘上的启动器 EXE/JAR、系统地区、时区、注册表或全局 Java 环境变量。运行时载荷使用内容哈希文件名，核对完整字节后原子发布；损坏缓存不执行。
- 原生、托管与 Java 初始化必须分开判断；“DLL 已加载”“Hook 已成功”“回调已恢复”“JVM agent 已转换”不是同一件事。

## 文件与机制

| 文件 | 作用 |
| --- | --- |
| `src/universal_proxy.cpp` | 文件名选择系统库、绝对系统路径转发、后台原生初始化与诊断 |
| `src/hook.cpp` | GeoName、DeviceRegion、用户 Locale 和首选 UI 语言覆盖 |
| `src/proxy_exports.json`、`scripts/generate-proxy.py` | 名称/ordinal 导出并集和 x86/x64 GAS/MASM 尾跳转 |
| `src/pcl_restore.cpp`、`managed/PclDownloadRestore.cs` | 原版 PCL2 指纹校验、进入已有 CLR、WPF Dispatcher 委托恢复 |
| `src/hmcl_launcher.cpp`、`src/hmcl_archive.cpp` | x86 壳 CreateProcessW IAT 设置、JAR 目录识别、agent 参数注入 |
| `src/hmcl_agent.cpp`、`java/HmclRegionAgent.java` | 自有 JAR 释放、RegionUtils 的 Instrumentation 转换与结果验证 |
| `java/HmclClassAnalyzer.java` | 解析实际字节码、识别地区判定根、追踪缓存和访问方法、生成替换计划 |
| `scripts/build-release.ps1` | 重新生成两种载荷、编译双架构、测试并打包 |
| `scripts/package-release.py` | 校验 DLL 架构/许可、独立 JAR 清单/许可及与内嵌载荷的一致性 |
| `.github/workflows/release.yml` | tag 构建、测试、上传双 DLL 与独立 JAR 并发布 Release |

详细指纹、上游提交和历史发现见 `docs/MAINTENANCE.md`；HMCL 识别条件见 `docs/HMCL_ANALYSIS.md`。不要未经分析就把“最新版”视为相同实现。

## 原生维护重点

- `DllMain` 只做轻量状态、固定 DLL、事件/线程准备，以及 HMCL 候选壳的导入指针设置。不在其中安装 MinHook、加载系统实现、运行托管代码、哈希/释放载荷或等待工作线程。
- HMCL IAT 必须在壳进入主函数前设置，不能改回依赖后台线程时序的方案。映射 PE 的导出模块名或启动器名称只用于候选筛选；真正的 CreateProcessW 调用中验证附加 JAR 的中央目录，必须含 Launcher 与 RegionUtils 条目。不要恢复 HMCL EXE 大小、时间戳或哈希白名单。
- 只给 `java.exe` / `javaw.exe` 且 `-jar` 指向同一 EXE 文件身份的命令加 agent。保留原 JVM 参数、引号/反斜杠、创建标志、环境块、工作目录、返回值与 LastError；已有本项目 agent 时避免重复添加。
- 保留 stdcall/cdecl 修饰、所有参数寄存器与栈参数。x86 MASM 要 `/safeseh`；.def 中 cdecl 转发目标不手工添加前导下划线，stdcall 回调使用对应修饰。
- 保留诊断 ordinal 4096–4099，rundll32 四参数 Unicode 释放入口使用 4100。不要用无类型返回值掩盖缺失导出。
- DllMain 的路径缓冲区必须保持静态，避免 DLL_THREAD_ATTACH 在小栈线程上溢出。
- CLR COM 接口需有外部链接；不要移入匿名命名空间，否则 GCC 可能错误去虚化成纯虚调用。只使用已运行的 CLR，不调用 Start/Stop。扩展 `\\?\` 路径传给 Framework hosting 前需规范化。

## 源码或上游发生变化时

1. 先记录用户提供文件的大小、SHA-256、PE 位数和实际进程树；不能从 EXE 文件名推断实际应用/JVM 版本。
2. 优先读取用户提供的反编译目录或本地源码。需要上游时只采用官方仓库/文档，固定 commit 并分别记录发布日期、提交日期、目标包版本；不要把 main 的变更当成本地包已经包含的实现。
3. 分别追踪“启动壳 → DLL 搜索 → 实际 CLR/JVM → 信号读取 → 缓存 → 回调/控件禁用”。先确定失败在哪一层，再修改相应代码。
4. PCL-CE 优先复核原 reader/evaluator、Locale/UI、注册表读取与 DLL 搜索标志；原版 PCL2 需复核 MVID、完整回调 IL、闭包字段、Dispatcher 和真实下载 loader；HMCL 需复核壳导入、CreateProcessW 参数、CodeSource、RegionUtils 方法及 Java 缓存。
5. HMCL 在类加载时解析真实字节码，从机器/网络判定根追踪缓存与访问方法；不按固定方法名或 EXE/class 哈希选择方法。修改识别规则时必须解释实际调用、字段与返回语义，并加入改名正向样例和易误识别的负向样例。SHA-256 仍用于自有载荷完整性及日志诊断。PCL2 的 EXE 哈希/大小在 native，MVID 与完整 IL 在 C#，继续保留这些校验。
6. 不要只靠字符串存在或放宽校验来宣称支持新版本。必须核对实际指令引用、调用链和失败后行为；HMCL 判定根缺失、多候选、缓存有歧义或不支持的结构应返回原始类并记录拒绝原因。取反、无关业务方法不能被一并替换。
7. 修改载荷后运行完整构建脚本。`src/managed_payload.h`、`src/hmcl_agent_payload.h` 为自动生成文件，不手写、不提交。Java 来源校验覆盖排序后的全部 `java/Hmcl*.java`，新增分析辅助类也必须纳入；CMake 会拒绝来源哈希过期的头文件。

## 验证与报告

```powershell
.\scripts\build-release.ps1 -JavaHome 'C:\path\to\jdk-21'
```

默认运行所有当前 CTest 与 Java 自动分析测试，包括两套不同方法/字段名的启动转换和 Attach 重转换，以及同名伪类、多判定根拒绝。当前为 14 组 CTest、6 组 JVM 场景。单独回归已构建工程：

```powershell
ctest --test-dir build/msvc-x64 -C Release --output-on-failure
ctest --test-dir build/msvc-Win32 -C Release --output-on-failure
```

- .NET Framework 的旧 csc 需要原生 Windows 路径；不要给它传 `C:/...` 形式的绝对源码路径。
- 每个代理模式要用新进程测试。x64 Python 无法载入 x86 DLL，x86 使用 `/platform:x86` 的 C# 探针。旧工程里 `GetUserDefaultLocaleName(NULL, 6)` 基线会崩溃；本仓库不包含该非法指针测试，不要用它判断代理失败。
- WinHTTP/WinMM 的系统 ordinal 随 Windows 版本可能变化，测试从当前已加载系统 DLL 的导出表取得实际 ordinal；不要把 26200 的名称快照当作 CI Windows Server 的 ordinal 真值。
- GUI/自动注入变更还需真实启动器验证。用隔离副本及测试数据，同时放入最终双 DLL，再直接运行原 EXE；确认实际模块路径、真实 JVM/CLR 和原生日志，不能只验证显式 LoadLibrary 或带 agent 的辅助脚本。
- HMCL 检查实际 JVM 命令中恰好一个 agent、日志 `ANALYZED` 的动态方法计划和 `VERIFIED: machine=false; network=false`、下载页搜索启用和真实版本列表。覆盖改名/PE 时间戳变化、中文空格路径、自定义参数、已有 agent，以及不匹配归档与损坏自有 JAR 的负向场景。原生归档解析还需测试边界、前置 PE、注释及同名目录条目。
- PCL2 / PCL2-CE 应确认旁边的 x86 version.dll 未被选中、本地 x64 winmm.dll 实际加载，并验证 reader 或真实下载回调；不要把读取成功或缓存列表等同于完整安装完成。
- 保留原始 EXE 的哈希、测试 DLL 哈希、日志与控件/窗口证据。仅关闭自己启动且已核实身份的测试进程，不关闭用户现有实例。
- CI 不包含真实启动器二进制或 GUI 流程。明确区分自动测试、实际启动器运行及完整游戏安装的验证范围，不宣称未执行的检查通过。

## 发布维护

tag push 才发布；手动 workflow_dispatch 只构建 artifact。两个架构必须在同一流程中使用同一批载荷，独立 JAR 直接取自该批 agent 输出；打包后检查 PE 位数、JAR 完整性及载荷一致性，保留源码/MinHook 许可。artifact、下载校验和 Release 上传清单必须同时包含三份产物。只在所有构建与测试通过后上传并公开 Release；已有公开 Release 不覆盖，新代码用新 tag。仓库写入权限仅授予发布 job，不需要 PAT。

不得提交目标启动器、反编译缓存、个人绝对路径、运行时日志/数据、账号信息或构建输出。源码根目录和发布的三种文件名是维护边界。
