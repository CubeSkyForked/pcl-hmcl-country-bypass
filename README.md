# HKRegionHook

面向 Windows 上 PCL2、PCL2-CE 和 HMCL 的启动器地区兼容补丁。

通过 DLL 代理、系统 API Hook 和运行时方法替换，调整启动器进程内的地区判定，恢复受其影响的下载功能。将两个 DLL 放入启动器目录即可使用，所需的托管助手和 Java agent 均已内嵌。

## 功能

- **统一部署**：同一组 `winmm.dll` 和 `version.dll` 可一起放入启动器目录，分别供已验证的 PCL2 / PCL2-CE 与 HMCL 加载。
- **进程内地区覆盖**：覆盖地区、用户 Locale 和首选 UI 语言等信号，返回香港地区相关数据；系统时区保持原值。
- **PCL2 下载回调恢复**：对已匹配的原版 PCL2，在已有 CLR 中恢复原下载委托，继续使用启动器自身的版本列表加载器。
- **HMCL 启动时自动分析**：自动向实际 JVM 加载 agent，按目标类的字节码、调用和缓存关系识别地区判定方法，适应方法与字段改名。
- **独立 Java agent**：Release 同时提供 `HKRegion.HMCL.Agent.jar`，高级用户可自行通过 JVM 参数加载。
- **运行时生效**：处理发生在启动器进程内，原始 EXE / JAR、系统地区、注册表和全局 Java 环境变量均不改写。

## 快速开始

1. 从 GitHub Releases 下载 `winmm.dll` 和 `version.dll`。
2. 完全退出启动器，将**两个文件一起复制到启动器 EXE 所在目录**。
3. 直接运行原启动器 EXE。

```text
启动器目录/
├── 启动器.exe
├── winmm.dll       # x64，供 PCL2 / PCL2-CE 使用
└── version.dll     # x86，供 HMCL 的 32 位启动壳使用
```

无需手动运行注入脚本或准备 Java agent。HMCL 按**启动壳的位数**选择 DLL，32 位壳可以启动 64 位 JVM，因此这里使用的是 32 位 `version.dll`。

启动器目录需要可写，以便首次启动时释放载荷到 `HKRegionCache`。更新补丁时，先退出启动器再替换两个 DLL；卸载时，退出启动器后删除这两个文件即可，生成的缓存和日志也可一并清理。

自动加载面向 Windows EXE 启动入口；直接使用 `java -jar` 运行 HMCL 不会经过 DLL 代理。

### 高级用法：独立 Java agent

从同一 Release 下载 `HKRegion.HMCL.Agent.jar`，使用 Java 17+ 启动 HMCL，将 `-javaagent` 参数放在 HMCL 的 `-jar` 之前：

```powershell
java "-javaagent:C:\tools\HKRegion.HMCL.Agent.jar" -jar "C:\launchers\HMCL.jar"
```

将示例中的路径替换为实际文件路径。此模式无需 DLL，agent 与 JVM 位数无关；它只处理 HMCL 的 Java 地区判定，不提供 PCL2 / PCL2-CE 的原生 Hook。

默认日志写在 agent JAR 所在目录的 `HKRegion-hmcl-agent.log`。也可以在 agent 参数后指定日志文件：

```powershell
java "-javaagent:C:\tools\HKRegion.HMCL.Agent.jar=C:\logs\hmcl-agent.log" -jar "C:\launchers\HMCL.jar"
```

## 兼容范围

| 启动器 | 已验证版本 | 加载入口 | 处理方式 |
| --- | --- | --- | --- |
| PCL2 | 2.99 | x64 `winmm.dll` | 系统 API 覆盖 + 托管下载回调恢复 |
| PCL2-CE | 2.15.1 | x64 `winmm.dll` | 系统 API 覆盖 |
| HMCL | 3.17.0.362 | x86 `version.dll` | 自动加载 Java agent，分析并替换地区判定方法 |

原版 PCL2 的托管恢复保留 EXE、MVID 和完整 IL 校验，只对匹配的实现生效。HMCL 依据实际类结构识别，已取消 EXE / class 哈希及固定方法名白名单；目标类位置或判定实现超出分析规则时会跳过转换。

表中的 DLL 加载入口已在对应版本验证。启动器更新后，加载方式与判定逻辑仍可能变化，具体样本、验证记录和升级分析见 [维护说明](docs/MAINTENANCE.md)。

## HMCL 启动时自动分析

HMCL 的地区检查运行在独立 JVM 中，由内嵌 Java agent 在目标类加载时处理：

1. 原生层识别 HMCL 启动壳及附加 JAR 结构，在创建实际 JVM 时自动加入一个 `-javaagent` 参数，并保留原有启动参数。
2. agent 解析 `org/jackhuang/hmcl/util/RegionUtils` 的实际字节码，识别机器与网络地区判定的计算方法。
3. 沿静态初始化、缓存字段和方法调用关系找到相关访问方法，动态生成替换计划，将所选判定方法改为返回 `false`。
4. 记录分析结果并调用识别出的公开访问方法，验证机器和网络地区判定均为 `false`。

方法数量与名称由实际分析结果决定。缺少特征、存在多个候选或缓存关系有歧义时，会保留原类并记录 `REJECTED`。分析规则和测试说明见 [HMCL 自动分析](docs/HMCL_ANALYSIS.md)。

## 日志与排查

使用 DLL 自动加载时，日志位于启动器 EXE 同目录：

| 文件 | 用途 |
| --- | --- |
| `HKRegion-hmcl-launcher.log` | HMCL 原生层载荷准备及 JVM 参数注入结果 |
| `HKRegion-hmcl-agent.log` | Java 目标类分析、转换和验证结果 |
| `HKRegion-managed.log` | 原版 PCL2 托管下载回调恢复结果 |

HMCL 正常转换后，agent 日志会包含以下内容，其中方法名和数量取自实际分析：

```text
ANALYZED: source=...; machine=[...]; network=[...]
TRANSFORMED: methods=...; retransform=false; inputSha256=...
VERIFIED: machine=false; network=false; ...
```

遇到问题时，请附上启动器版本、复现步骤及对应日志。DLL 成功加载后，还需检查实际 JVM 的 agent 日志或 PCL2 的托管恢复日志，以确认后续处理是否成功。

## 从源码构建

### 环境要求

- Windows x64。
- Visual Studio 2022，安装 C++ 桌面开发工具、x86 / x64 编译器、MASM 和 Windows SDK。
- CMake 3.24+、x64 Python 3。
- JDK 17+，CI 使用 JDK 21。
- .NET Framework 4.x 的 C# 编译器与 WPF 程序集。

在仓库根目录运行：

```powershell
.\scripts\build-release.ps1

# 显式指定 JDK
.\scripts\build-release.ps1 -JavaHome 'C:\path\to\jdk-21'
```

构建脚本从源码生成 C# 助手与 Java agent，再编译 x64 和 Win32 DLL、执行测试并校验打包结果。成品位于：

```text
dist/
├── winmm.dll                 # x64
├── version.dll               # x86
└── HKRegion.HMCL.Agent.jar    # Java 17+，可选的独立 agent
```

`build/release-manifest.json` 记录文件架构、大小与 SHA-256；`build/release-notes.md` 提供发布说明。修改 C# 或 Java 源码后，重新运行完整构建脚本即可同步载荷；生成的头文件和构建输出不纳入源码。

### 测试

完整构建默认运行 14 组 CTest 和 6 组 JVM 场景，覆盖 API 转发、32 / 64 位 ABI、地区 Hook、载荷释放、归档识别，以及 HMCL 方法 / 字段改名后的启动转换、Attach 重转换和歧义拒绝。

已构建工程可单独回归：

```powershell
ctest --test-dir build/msvc-x64 -C Release --output-on-failure
ctest --test-dir build/msvc-Win32 -C Release --output-on-failure
python tests/hmcl/run-analysis-tests.py --java-home 'C:\path\to\jdk-21'
```

HMCL 自动分析还通过了原始 EXE、改名并修改 PE 时间戳的 EXE 副本，以及后续上游提交目标类的独立探针测试。自动测试使用自有测试类，不分发启动器二进制；真实下载页与完整游戏安装的验证范围分别记录于 [维护说明](docs/MAINTENANCE.md)。

## 自动发布

[GitHub Actions 工作流](.github/workflows/release.yml) 在推送 tag 时构建并测试两个架构，随后创建 Release，上传 `winmm.dll`、`version.dll` 和 `HKRegion.HMCL.Agent.jar`。独立 JAR 与两个 DLL 内嵌的 agent 来自同一次构建，打包时校验字节一致。

```powershell
git tag v1.7.0
git push origin v1.7.0
```

工作流使用仓库自带的 `GITHUB_TOKEN`。已有公开 Release 不覆盖，新版本使用新 tag；手动触发 `workflow_dispatch` 只生成构建 artifact。

## 项目结构

```text
src/                    原生 Hook、DLL 代理与载荷加载
include/                原生接口定义
managed/                PCL2 托管下载回调恢复
java/                   HMCL agent 与字节码分析器
scripts/                载荷生成、构建和打包
tests/                  原生接口与 JVM 回归测试
third_party/minhook/    MinHook 源码及许可
docs/                   兼容性分析和维护文档
.github/workflows/      构建与 Release 自动发布
```

参与维护前请阅读 [AGENTS.md](AGENTS.md)。变更地区判定或加载逻辑时，应同时更新相关测试与兼容性记录。

## 许可证

项目代码采用 [MIT License](LICENSE)，独立 agent JAR 在 `META-INF/LICENSE` 中包含项目许可证。内嵌的 MinHook 遵循其 [BSD 2-Clause 及 HDE 许可声明](third_party/minhook/LICENSE.txt)，两份 Release DLL 均包含相应许可资源。完整第三方说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
