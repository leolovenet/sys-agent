# sys-agent — AGENTS.md

本文件是 `src/sys-agent` 的专属规则;它是通用 Switch 工具箱 sysmodule,属于工作区(`/Users/leo/Documents/switch 金手指`)的嵌套独立 Git 仓库。环境级事实(Docker 镜像、keys 政策、hosts)与通用纪律见根 `AGENTS.md`。

## 范围与哲学

- sys-agent 是**通用 Switch 工具箱**(见 README "Project scope and philosophy")。命令保持进程与游戏无关;游戏专属偏移、hook 布局与签名属于消费方项目。
- `debug patch-code` 是通用事务性代码补丁原语:任意进程(经 `pid=`),载荷 ≤ 1 KiB,pause/verify/write/readback/resume,保证恢复。普通数据 poke 保持不暂停。

## 仓库边界

- `origin` = `https://github.com/leolovenet/sys-agent.git`(fork);`upstream` = `https://github.com/olliz0r/sys-botbase.git`(只读)。
- 只向 fork 与分支提交/推送;向上游贡献走 GitHub PR,绝不直接推官方 upstream。
- `third_party/ftpsrv` submodule 固定 commit `7c82402e8f9a53400ea33b82eebd961dfa83a422`(MIT);构建前用递归 submodule 克隆/更新。

## 构建

```bash
docker run --rm \
  --platform linux/amd64 \
  -v "/Users/leo/Documents/switch 金手指/src/sys-agent:/work" \
  -w /work \
  devkitpro/devkita64:20260219 \
  bash -lc 'source /opt/devkitpro/switchvars.sh && make'
```

- Docker socket 沙盒受限,需 escalation;固定镜像已零警告编译本地 commit `ae13548c587b75abf54f32032ad8d52f51eda05e` 与上游 v2.5 tag `45b6a6419624ed1a1f34aabc9f37336d5f130e61`。
- 构建成功不等于部署授权:安装/测试自定义 sysmodule 需要明确请求、版本/ABI 检查与分阶段真机测试;旧备份不保留(policy 2026-08-31),回滚 = 重建重部署。
- 发布产物是**本地产物**(`build/` 被忽略,不进仓库):发布说明写 `build/release/v<版本>-notes.md`(正文格式见 `v2.7.6-notes.md`),SD 布局放 `build/release/v<版本>/atmosphere/contents/43000000000000A6/{exefs.nsp,toolbox.json,flags/boot2.flag}`,再压成 `build/release/sys-agent-v<版本>.zip`。

## 协议约定

- 字节载荷只接受 hex 字节对(可选 `0x` 前缀;奇数长度或非 hex 字符拒绝,错误码 `INVALID_HEX_PAYLOAD`)。
- 普通数字是 `0x`/`0X` hex 或十进制;非法字符串解析为 0,而不是静默部分值(`12AB` 不是 hex — 写 `0x12AB`)。
- 未知按钮 token 不发任何按钮,绝不发默认键。
- 写命令前必须回环检查服务器错误码。
- hex 载荷端到端 hex 解析(验证解析器,不只验证发送方)。历史教训:只有带 `0x` 前缀才按 hex 解析,导致 `50 00 00 58...` 被写成十进制乱码。
- 虚拟手柄(`click`/`press`/`release`/`setStick`/`clickSeq`)在空闲 ≥0.5s 后按布局指纹重建设备以熬过底座/Joy-Con 拓扑变化,attach 后等到"玩家1 已登记我们的 pad"再写状态(下限 50ms/上限 150ms,实测约 50ms),用完让位(默认 1000ms,`controllerIdleRelease` 秒 / `controllerIdleReleaseMs` 毫秒;这是"真手柄下线"里唯一由我们掌握的时长)并按蓝牙地址主动回连(`btdrvTriggerConnection`):虚拟设备还占着玩家槽时**完全不触发**(占槽时的寻呼会被接受但不会注册成手柄,白耗预算),让位后适配器一次只寻呼一条链路,同一对 Joy-Con 的第二只要等第一只链路建立完,实测 0.5~1.3s 内一律返回链路忙(`0x2F4471`),所以失败地址按主循环节拍 50ms 重试、预算 40 次(≈2s),再 0.25s/0.6s 兜底;机制、时间阀、诊断命令与验收见 `docs/hdls-virtual-controller-notes.md`。**`hiddbgApplyHdlsNpadAssignmentState` 在 22.1.0 上崩过机,命令与自动路径都已删除,不得再加回来。**

## 运算与字节序纪律

- 一律用工具计算地址/偏移/编码;明确基址是 `main`、`rtld` 还是模块基址。
- AArch64 小端,内存为字节序。`debug patch-code` 载荷与 peek/poke 字节用内存序(最左 = 最低地址);指令载荷 = `struct.pack('<I', word).hex()`,patch 前 objdump 目标并与现场字节比对,不一致就中止。
- 反汇编结论只来自 `aarch64-none-elf-objdump` / LLVM objdump,不要手解 4 字节指令。
- sysmodule 只能对自己地址空间做缓存/内存操作;目标进程地址必须经调试接口(svcRead/WriteDebugProcessMemory),绝不直接指针/缓存操作。

## FTP 服务

- 内置低优先级 FTP 服务,端口 `6001`,匿名、默认开启;可选配置 `/config/sys-agent/ftp.ini`;端口 `6000` 上 `ftpStatus/ftpStart/ftpStop/ftpRestart/ftpReload` 控制。根只暴露 SD,不含 BIS/存档/卡带等挂载。
- 稳定测试基线文件名是含空格的 ASCII;中文/日文名返回原生 FS Result `0x202`,其他非 ASCII 枚举不一致;不要静默重命名这类路径。
- 已通过真机测试:FTP CRUD、中断清理、16 MiB hash 校验传输、续传、生命周期命令、6000 端口响应、`RNTO` 覆盖已存在文件。冷启动重试现在可观测:`ftpStatus` 暴露 `listener`/`bindAttempts`/`lastBindError`,重启后实测 `bindAttempts=3` 后监听成功;曾报"6000 通、6001 不通"未复现(实测到的是整机掉线,两端口同时恢复),该项保留待复现。**未关闭验收**:FTP 未与活动 C-level 搜索并发测试(含 `/switch/sys-agent/search` 写保护)。验证矩阵见 `docs/ftp-server.md`。
- 替换运行中的 sysmodule:FTP 上传需 ASCII 临时名 + 原子重命名,备份 `atmosphere/contents/43000000000000A6`,然后整机重启;只重启游戏不会重载 sys-agent。

## Headless 启动与 Keys

- `game launch-headless <titleId>`:客户端解析更新的 rights id,读 `SDcard/switch/title.keys` 的自定义行(`rightsId = <title_key_block hex> <keygen>`),请求 sysmodule(`gameExternalKeyPrepareCommon`)经 `spl:es PrepareCommonEsTitleKey` 计算当前启动的 AccessKey 并注册(fsp-srv 607),设置 lr redirect base→patch,启动更新构建(ACNH 3.0.3 已验证,任何启动都不用手动开游戏)。
- block 缺失时用 `gameExternalKeyScan <rightsIdHex> fs` 在游戏运行时扫描 fsp-srv 内存恢复;NPDM 含调试 syscall(`svcDebugActiveProcess` 等),可直接读 fsp-srv/es 内存。
- 完整机制见 `docs/headless-launch-rights-key-notes.md`(改 launch/keys 前必读)。永不打印 key 值。

## 内存后端

- 统一 `ProcessMemoryBackend` 默认 `auto`,用 Atmosphere `dmnt:cht`,可与 ACNH cheat 共存;用 `memoryBackendProbe` 验证并要求 `active=dmnt`。
- 官方/旧构建与显式 `direct` 模式自行调用 `svcDebugActiveProcess`,在 dmnt 持有调试句柄时可能 `0xF401` 或 `getHeapBase=4`。
- 只测 direct 路径时:临时重命名 `/atmosphere/contents/01006F8002326000`,重启游戏,测试后恢复。

## 搜索

- 协议-v3 C 级搜索:256 KiB SD 后备块 + 20 ms 块间 yield(NPDM 把所有线程限制在 CPU 3)。
- 真机验证(ACNH 3.0.3 + 活动 Atmosphere cheats):128 MiB `u32` 堆快照 27.9 s、28,472,320 候选、零读错误;正常状态延迟低;最终同步 SD flush 有一次约 2.3 s 延迟尖峰。
- 不要在没有重复并发 TCP、手柄、取消与 128 MiB 测试的情况下移除 freeze-worker 检查或减少 yield。

## 系统管理命令

- 端口 `6000` 暴露未认证系统管理命令。Mariko `systemRebootEmuMMC` 经 Hekate `id=Atm-Emu` 验证;Erista 路径与 `systemSleep` 仍实验性。`networkProfile` 会暴露当前 Wi-Fi 口令。

## 文档导航

| 路径 | 内容/何时读 |
|---|---|
| `README.md` | 项目范围、哲学与命令概览 |
| `commands.md` | 全部可用命令参考 |
| `client/README.md` | sys-agent client 说明 |
| `third_party/README.md` | 第三方源码清单 |
| `docs/ftp-server.md` | FTP 服务与验证矩阵(改 FTP 前) |
| `docs/hdls-virtual-controller-notes.md` | HDLS 虚拟手柄失效机制、自动重建/自愈、诊断命令与系统限制(改输入或手柄代码前必读) |
| `docs/headless-launch-rights-key-notes.md` | headless 启动与 external key 机制(改 launch/keys 前必读) |
| `docs/process-memory-backend.md` | 统一内存后端设计(direct/auto) |
| `docs/search-a-level-design.md` | A 级精确搜索设计与部署门槛 |
| `docs/search-c-level-design.md` | C 级未知值搜索设计 |
| `docs/research/audio-control.md` | 系统音量控制研究(`aud:ctl`/`audctl`) |
| `docs/research/client-roadmap-and-edizon-gap.md` | 客户端路线图与 EdiZon-SE 能力差距 |
| `docs/research/game-lifecycle.md` | 游戏启动/关闭/暂停研究 |
| `docs/research/两代switch芯片.md` | Erista/Mariko 芯片平台说明 |
