# HMCL 启动时自动分析

`HmclRegionAgent` 在 `premain` 注册转换器。目标类 `org/jackhuang/hmcl/util/RegionUtils` 加载时，`HmclClassAnalyzer` 解析 JVM 提供的 class 字节，识别判定计算、缓存字段和访问方法，再生成替换计划。HMCL EXE/class 的哈希、大小、PE 时间戳和旧版四个方法名均不参与选择；日志中的输入 SHA-256 用于复现问题。

## 选择规则

1. 解析常量池、字段、方法及 Code，逐条解码指令。只有实际指令引用的字符串、调用和字段用于分析，常量池中孤立出现的关键词不能作为证据。
2. 在有实现的静态 `()Z` 方法中识别判定根。机器根需读取系统时区，引用 `Asia/Shanghai`，并有 `CN` 或 `GetUserGeoID` 特征。网络根需有实际网络读取调用、`CN`，以及国家代码字段/访问器或 ipwho.is 地址特征。每种根必须恰好一个。
3. 沿 `<clinit>` 中机器根的直接赋值追踪 boolean / Boolean 静态缓存；沿网络根的直接调用者追踪网络缓存写入。网络缓存有多个候选时拒绝。
4. 识别读取已确认缓存的简单 getter、直接返回已确认判定的别名，以及仅操作这些判定/缓存的网络缓存包装方法。包装方法可分支、同步、装箱和拆箱；包含取反算术、true 常量、对象创建或无关调用时拒绝该方法。两种判定都必须存在可用于验证的公开访问方法。
5. 仅将所选方法的 Code 改为 `iconst_0; ireturn`。方法数量由结果决定；常量池、字段、签名及类级属性保留。

这些规则针对目前研究过的 HMCL 判定实现。若目标类改名、判定移入其他类、网络接口变化或缓存逻辑超出上述模式，需要继续分析并扩展规则。自动分析并不保证所有未来版本都能识别。

根缺失、多个根、缓存歧义或解析失败时，转换器返回 `null`，保留原类，并记录 `REJECTED`。同名同方法签名的普通类也必须经过这些结构检查。

## 启动与诊断

x86 原生壳先按映射 PE 的导出模块名或 HMCL 文件名前缀识别候选，在主函数前设置 CreateProcessW IAT。真正创建 JVM 时再解析 EXE 附加 JAR 的中央目录，要求有精确的 Launcher.class 与 RegionUtils.class 条目，仅对 `java.exe` / `javaw.exe` 且 `-jar` 指向当前 EXE 文件身份的命令加入自有 agent。原生识别目前支持普通单卷 ZIP，拒绝 ZIP64、多卷及超出边界的目录。

正常运行仍只需 x64 `winmm.dll` 和 x86 `version.dll`。自有 agent JAR 的完整性校验、原子释放和缓存内容哈希继续保留。PCL2 的 EXE / MVID / IL 校验也继续保留。

### 独立 agent

Release 同时提供 `HKRegion.HMCL.Agent.jar`，与 DLL 内嵌的 agent 字节一致。使用 Java 17+，可直接向运行 HMCL 的 JVM 添加参数，无需原生启动壳或 DLL：

```powershell
java "-javaagent:C:\tools\HKRegion.HMCL.Agent.jar" -jar "C:\launchers\HMCL.jar"
```

`-javaagent` 必须位于 HMCL 的 `-jar` 之前。agent 默认将日志写在自身 JAR 所在目录，也可使用 `-javaagent:<agent路径>=<日志路径>` 指定位置。该模式执行相同的字节码分析和转换，仅作用于 HMCL 的目标 Java 类。

### 验证日志

`HKRegion-hmcl-agent.log` 包含：

```text
ANALYZED: source=...; machine=[实际方法名...]; network=[实际方法名...]
TRANSFORMED: methods=实际数量; retransform=false; inputSha256=...
VERIFIED: machine=false; network=false; timezone=Asia/Shanghai; locale=zh-CN
```

Attach 模式会重新分析已加载类的实际字节码并重转换，支持缓存已初始化的情况。转换器及重转换约束遵循 [ClassFileTransformer 文档](https://docs.oracle.com/en/java/javase/21/docs/api/java.instrument/java/lang/instrument/ClassFileTransformer.html)。不会直接改写 static final 字段；已创建的禁用控件可能仍需重新进入页面。

## 回归测试

完整构建自动运行 `tests/hmcl/run-analysis-tests.py`。也可在已生成 agent 后单独运行：

```powershell
python tests/hmcl/run-analysis-tests.py --java-home 'C:\path\to\jdk-21'
```

- 两套自有 RegionUtils 样例分别使用不同方法、字段、网络地址和国家代码访问方式，均验证启动转换及缓存已初始化后的 Attach 重转换。
- 直接别名会一起转换；无关业务方法、取反方法和 switch 代码保留。启动转换不执行原地区网络查询。
- 多个机器判定根、只有旧方法名而没有对应指令特征的伪类均拒绝转换。
- 原生归档测试覆盖普通 JAR、前置 PE、ZIP 注释、缺少目标条目、相似条目名、截断及只有标记文本的文件。

CI 使用自有测试类，不分发 HMCL 类或启动器。真实 GUI 回归需同时放入最终双 DLL，直接启动隔离副本，检查自动注入、分析计划、验证日志及下载页；EXE 改名和 PE 时间戳改变也应复测。
