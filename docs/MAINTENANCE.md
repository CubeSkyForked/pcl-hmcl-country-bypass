# 兼容范围与后续分析

## 双 DLL 加载关系

| 目标 | 当前实际进程 | 进入方式 | 后续处理 |
| --- | --- | --- | --- |
| PCL2-CE 2.15.1 | x64 .NET 10 | `winmm.dll`，动画时钟的 timeSetEvent/timeKillEvent | 覆盖原系统 API 信号 |
| 原版 PCL2 2.99 | AnyCPU 包在测试系统以 x64 Framework 4.8 运行 | `winmm.dll` | 香港信号 + 已运行 CLR 中恢复下载委托 |
| HMCL 3.17.0.362 | x86 原生壳，独立 x64 Java 21 | `version.dll`，壳直接导入版本函数 | 创建实际 JVM 时自动传入 agent |

当前 PCL2-CE 的 CoreCLR 版本查询只搜索系统目录；网络走 SocketsHttpHandler，未加载本地 winhttp；WinMM 入口可以加载本地代理。原版 PCL2 与 HMCL 的双文件组合也应在源码或运行时变化后重新验证。这是具体应用的加载行为，不是 Windows 保证所有 x64 程序都会跳过旁边的 x86 version.dll。

同一架构的 DLL 内部具有 version/winhttp/winmm 导出并集，但正式交付只选表中两个名称。系统库以系统目录绝对路径加载，不能把代理复制进 System32/SysWOW64。

## PCL2-CE：原始系统信号

分析固定 [PCL-CE commit 1a765312](https://github.com/PCL-Community/PCL-CE/tree/1a7653127d578efdb2a9388999a3fdc67d9e49d5)。重点路径是 `PCL.Core/App/Region/RegionPolicy.cs` 和 `PCL.Core/UI/Animation/Clock/WinMMClock.cs`。

| 信号 | 进程内覆盖 |
| --- | --- |
| GetUserDefaultGeoName | HK |
| HKLM 指定 DeviceRegion 值 | 已存在且可读的 DWORD 返回 104 |
| GetUserDefaultLocaleName | zh-HK |
| GetUserPreferredUILanguages | 单项 zh-HK；ID 格式为 0C04 |
| 时区 | 不修改 |

目标键：`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Control Panel\DeviceRegion`，值名同为 `DeviceRegion`。不创建或改写系统值，处理 RegGetValue/RegQueryValueEx A/W、键身份、视图和缓冲区语义。只改 GeoName/DeviceRegion 仍可能被“中国时区 + zh-CN Locale/UI”分支限制，所以 Locale/UI 也是必要信号。

## 原版 PCL2：Framework 回调

- 精确样本大小：5,980,160 字节。
- EXE SHA-256：`6b6e1260cb5a1d6a303855f1c60cab3d0bc1aebba7e40a964180188b271e7386`。
- MVID：`926d447e-daff-4169-8c64-adc3d37d5f78`。
- 原阻止回调 token：`06001468`，完整 IL：`72 eb 45 00 70 73 a6 01 00 0a 7a`。

这是固定抛异常的托管下载回调，单改地区 API 无法恢复。native 先核对整个 EXE，再释放自有 C# 助手并进入已运行 Framework CLR。助手核对 MVID、完整 IL，在 WPF Dispatcher 上替换 `PageDownloadInstall` 闭包缓存中的下载委托，转接原有 DlClientListLoader。失败页面持有的旧委托也需处理，不能只改静态缓存。

已验证版本列表可恢复并显示 920 项；这不代表整合包入口或完整游戏安装已验证。后续分析优先检查闭包结构、原 loader、IsForceRestarting 字段、取消参数与页面刷新路径。不要直接跳过所有异常处理或创建第二个 CLR。

## HMCL：壳与实际 JVM

- 上游：[HMCL-dev/HMCL](https://github.com/HMCL-dev/HMCL)。本地 362 对应 [583e004](https://github.com/HMCL-dev/HMCL/commit/583e004d718102ae4489aa3f64bcde0c0161d000)。后续 [8827fd0](https://github.com/HMCL-dev/HMCL/commit/8827fd06d8ea8bcd899e54f76004f63a447e2601) 优化时区排除，不能视为本地 362 已包含。
- EXE 大小：12,114,419 字节；SHA-256：`08ba3e5435b51ad5d8d5266acb8cfac88736c4b5afb9c30f6b6455565d0f8652`。
- 壳 PE：i386，TimeDateStamp `0x68efa46a`，SizeOfImage `0x73000`。壳文件版本 3.7.0.1；实际 JAR 版本 3.17.0.362。
- 目标类：`org/jackhuang/hmcl/util/RegionUtils`。原 class SHA-256：`f438e7685183f71eb476d2e78dd93aaff779867df102265d91e3a4e33c16ac08`。

以上大小、时间戳和哈希是本地样本的诊断记录，1.7 起不作为 HMCL 运行时白名单。其他版本能否识别取决于实际类结构；目标类路径或判定实现发生根本变化时仍需更新分析规则。

机器判定先检查 Asia/Shanghai，再检查 UTC+8 的 Locale CN 或 Windows JNA GetUserGeoID=45。机器判定为 true 才查询 ipwho.is；CN、失败或抛异常都视为大陆网络并缓存。因此壳内的系统 Hook 不会改变独立 JVM 的 Java 缓存，也无法覆盖第一条直接时区分支。

agent 在 `RegionUtils` 实际加载时分析指令引用，识别机器与网络判定根，再追踪静态初始化缓存、网络缓存写入与公开访问方法。方法和字段名从实际类中取得，不绑定旧版四个方法名。匹配后仅替换相关静态 `()Z` 方法的 Code 属性为返回 false，保留常量池、方法签名与类结构，不重新打包 HMCL 的类。当前 362 样本动态识别出四个方法；其他受支持结构的方法数可以不同。识别条件、拒绝行为与离线测试见 [HMCL 自动分析](HMCL_ANALYSIS.md)。

启动模式使用 premain，在类初始化前转换。Attach 重转换也重新分析 JVM 提供的实际字节码，不依赖原始常量池索引或 class 哈希，不直接写 static final 字段。已禁用的 JavaFX 控件不会因后加载 agent 自动重建，需重进页面或重启。

### 自动传入 agent

1. x86 version.dll 在 DllMain 按 PE 架构及导出模块名/文件名筛选候选壳，并设置 CreateProcessW 导入指针，防止后台线程安装太晚。
2. 壳真正创建 Java 时，在普通调用上下文解析附加 JAR 中央目录，核对 Launcher 和 RegionUtils 条目，再提取并核对自有 agent JAR；不检查固定 EXE 哈希。
3. 检查 Java 程序名、`-jar` 和目标文件身份；保留现有参数，在 `-jar` 前加入一个 agent 参数。
4. 原生日志出现 `child-created-with-agent; error=0`；JVM 日志先给出 `ANALYZED` 方法计划、`TRANSFORMED` 方法数，再记录 `VERIFIED: machine=false; network=false`。分析失败记录 `REJECTED` 并保留原类。

参数按 [CommandLineToArgvW 的规则](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw) 处理，遵守 [CreateProcessW 的可写缓冲区要求](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)。不把哈希、文件释放或同步等待放进 [DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)。rundll32 释放入口保留供诊断，但用户正常启动不需要它。

缓存使用 `HKRegion.HMCL.Agent.<实际载荷 SHA256>.jar`。源码编译器变化可能改变 JAR 字节与文件名，所以不要把历史 agent 的字节数或载荷哈希当作固定目标指纹。自有载荷仍核对完整字节及 SHA-256；C# 助手同理按实际编译结果哈希命名。

## 验证边界

过去已实际验证：HK 系统信号、原版 PCL2 下载列表恢复、HMCL 无脚本直接启动及下载搜索/版本列表、中文空格路径、参数保留、已提供 agent 时去重、不支持的 EXE 拒绝。编译器或源码变更后要针对相应路径重做验证，不能把历史截图当成本次产物证据。

本仓库测试不分发或下载目标启动器；CTest 检查原生接口、ABI、归档结构、提取与拒绝行为，JVM 测试使用自有小型类验证选择规则。真实下载页需要用户提供原包，在隔离目录同时放入最终两个 DLL 后验证。只记录自己测试进程的必要模块、启动参数、日志和界面，不采集账号数据。未验证完整游戏安装、登录或联机功能。

## 2026-10-21 独立仓库首次验证（历史构建）

本机 Windows 11 26200，Visual Studio 2022 / MSVC 19.44.35211、SDK 26100、GraalVM CE JDK 21.0.2、Python 3.13。从本目录原始源码生成两种载荷并构建，x64 九组、x86 三组 CTest 与 Java 不支持归档测试通过；工作流通过 actionlint 1.7.12。GitHub 托管 runner 上的实际 tag 发布尚未运行。

| 最终文件 | 字节数 | SHA-256 |
| --- | --- | --- |
| winmm.dll（x64） | 334848 | `71d82e31418ebb35109b2807affa14d147bb9cbc048af09e868a9c9db74460a4` |
| version.dll（x86） | 247296 | `95132488998015609144e4f62a0c09c106f16a7ad3e229220c2e297ce7e53369` |

分别将这两份文件同时放入三款原始启动器的隔离副本目录，直接运行原 EXE：

- 原版 PCL2：本地 x64 winmm.dll 加载，version.dll 来自系统目录；新编译 C# 助手恢复下载回调，原 loader 返回 920 项。
- PCL2-CE：本地 x64 winmm.dll 加载，version.dll 来自系统目录；下载页显示 26.3，控件启用。
- HMCL：x86 壳自动加入一个 agent 参数，新的 JDK 21 编译载荷转换四方法成功，machine/network=false；下载页搜索启用并显示 26.3。

以上测试进程均正常退出，原 EXE 保持原始哈希。本次构建的载荷哈希会随编译器/工具链变化，后续构建以实际生成的 manifest 为准。

## 2026-10-21：1.7 自动分析回归

使用同一工具链从源码重新生成载荷，14 组 CTest（x64 十组、x86 四组）、6 组 JVM 自动分析场景通过。工作流通过 actionlint 1.7.12，构建 PowerShell 脚本语法检查通过。最终产物为：

| 文件 | 字节数 | SHA-256 |
| --- | --- | --- |
| winmm.dll（x64） | 349696 | `bdd3ebb41d6b7d7ae180dbc2abdb4bd141e1977f4d945286def38c8535815199` |
| version.dll（x86） | 265216 | `dd26ce305aa140694b49b2ba4b38b0c7261253bf10e25ce2de516ada555dfc38` |

同时放入这两份 DLL，分别直接启动原始 HMCL 3.17.0.362 副本，以及改名为含空格文件名、PE 时间戳改为 `0x7050ABCD` 的副本。后者 EXE SHA-256 为 `a16366402566754c1b083fe39e3abc3ea6b7efa90c6909a684ab68d216e1631d`，归档内容保持一致。两种场景均自动加入恰好一个 agent，动态识别四个相关方法，记录 machine/network=false；下载页搜索框与 26.3 条目启用。测试进程经窗口关闭请求退出，用户原始 EXE 未改动。

另从上游固定提交 [8827fd0](https://github.com/HMCL-dev/HMCL/commit/8827fd06d8ea8bcd899e54f76004f63a447e2601) 取得 RegionUtils 及两个编译依赖，使用本地 362 的其余依赖编译单独的 class 探针。该类 SHA-256 为 `e0dbbe3f0b4ace56a36ce6594d00fbee396cdd6de072e3c8533a049aebd52ad1`，与 362 原 class 不同；同一 agent 在 `-Xverify:all` 下动态识别并转换成功，两个公开地区判定返回 false。这是目标类兼容性检查，没有构建或启动该提交的完整 HMCL，也未把上游类纳入本仓库。

本轮实际 GUI 回归针对 HMCL；PCL2 / PCL2-CE 的 GUI 证据仍属于上面的历史构建。本轮未在 GitHub 上推送 tag 或执行托管 Release。
