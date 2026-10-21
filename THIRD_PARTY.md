# Third-party components

## MinHook v1.3.4

- Upstream: https://github.com/TsudaKageyu/minhook/tree/v1.3.4
- Unmodified required source: `third_party/minhook/src/`, `third_party/minhook/include/`.
- BSD 2-Clause and HDE notices: `third_party/minhook/LICENSE.txt`.
- Original archive URL and SHA-256: `third_party/minhook-source.json`.
- Statically linked into both release DLLs. The full notice is also embedded as RCDATA resource 102.

The hooks, proxy, C# helper, Java agent and build/test scripts are original project
code under `LICENSE`. The project license is embedded as RCDATA resource 101.
The standalone `HKRegion.HMCL.Agent.jar` includes this license as
`META-INF/LICENSE`; it contains no MinHook code.
CI uses MSVC with the static runtime; Microsoft compilers, SDKs, .NET and Java
runtimes are not bundled. No launcher executable, decompiled launcher class,
Minecraft asset, authentication data or downloaded application cache is included.

Source analysis references include PCL-CE, PCL2 and HMCL. The agent changes only
the in-memory regional predicates selected by class analysis; no HMCL class
is redistributed. See `docs/MAINTENANCE.md` for the audited upstream references.
