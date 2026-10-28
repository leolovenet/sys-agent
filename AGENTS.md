# sys-agent — AGENTS.md

本文件是 `src/sys-agent` 的专属规则;它是通用 Switch 工具箱 sysmodule,属于工作区(`/Users/leo/Documents/switch 金手指`)的嵌套独立 Git 仓库。环境级事实(Docker 镜像、keys 政策、hosts)与通用纪律见根 `AGENTS.md`。

## 范围与哲学

- sys-agent 是**通用 Switch 工具箱**(见 README "Project scope and philosophy")。命令保持进程与游戏无关;游戏专属偏移、hook 布局与签名属于消费方项目。
- `debug patch-code` 是通用事务性代码补丁原语:任意进程(经 `pid=`),载荷 ≤ 1 KiB,pause/verify/write/readback/resume,保证恢复。普通数据 poke 保持不暂停。
- `debug maps` / `debug threads` / `debug modules` / `debug read` / `debug wait-title` / `debug wait-application` 是通用的任意进程检查与生命周期原语;保持与游戏/具体系统服务无关,不要为单一调查加特例。
- `debug trace` 是通用的任意 PID 硬件执行 tracer:不修改目标代码,单会话最多 4 个目标,记录目标/线程/core/PC/LR/SP/参数/指令、命中时最多 2 个寄存器指针内存快照与固定 ring。命中后必须清除当前执行槽、对命中的线程硬件单步一条指令、恢复执行槽,再继续整个进程;省略这一步会让同一断点原地反复触发,永远到不了后续目标。ring 仍受默认 64 次总命中安全上限约束,只有显式 `hits 0` 才解除;实机表明 4096 次快速命中已足以触发 ACNH watchdog。它只记录显式配置的站点;客户端配对 entry/return 仍是离线分析,不是自动调用栈追踪。游戏偏移仍由消费方提供。
- `trace-dump` 的版本化 JSONL、`trace-analyze`、`trace-diff`、`trace-ipc` 与 `trace-timeline` 属于客户端离线关联层:只能对已显式记录的站点/快照做关联,不代表已实现自动 CMIF 语义发现。`trace-timeline` 仅在同一 boot epoch 内按系统 tick 合并多个会话。
- `trace-plan` / `trace-run` / `trace-report` 的 profile 层只做通用进程选择、Title/Build ID 校验、消费方符号表/主模块 offset 解析、每 target 可选命中时内存快照、`expectBytes` 校验和稳定 target ID 映射。`ipc-decode` / `trace-ipc` 只解析 HIPC/CMIF 字节，并按 profile pair 把 request/response 配成 transaction，不内置具体 interface 语义。ACNH、Account、SSL 等具体 offset/profile 必须由消费方提供,不得写入 sysmodule;schema 与限制见 `docs/trace-profiles.md`。
- `debug hwbp-layout <pid1> <pid2>` 是实验性硬件容量探针:短生命周期间时 attach 两个进程,只验证两个 context-IDR 句柄和可用执行槽是否可共存,不安装实际执行地址。结果用于决定同步多进程 trace 的架构,不要把它描述成已经支持 IPC trace。
- `debug multitrace` 是实验性同步双进程 trace:固定两个独立 PID、每进程最多两个目标地址,共享 ring 和总命中上限,事件用 target index 区分目标。两个执行站点可分别传函数入口与调用者返回地址,用于捕获真实返回 `x0`;命中时可读取最多 2 个寄存器指针快照。它本身不理解 CMIF/HIPC,消息解析位于客户端 profile/decode 层;一个目标退出时整个会话停止。

## 仓库边界

- `origin` = `https://github.com/leolovenet/sys-agent.git`(fork),同时是 `gh` 在本目录的默认仓库(`gh repo set-default leolovenet/sys-agent`)。上游 `olliz0r/sys-botbase` **不配 remote**:它只是只读参考,而把它配成 remote 会让 `gh` 在"需要挑一个仓库"的命令上默认选中它(例如 `gh release create` 会报 "tag exists locally but has not been pushed to olliz0r/sys-botbase");要读上游代码用 `gh repo view olliz0r/sys-botbase` 或直接 URL。
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
- **两条独立的版本线**:sysmodule 的 `VERSION_S`(`sys-agent/source/main.c`)与客户端的 `CLIENT_VERSION`(`client/sysagent.py`)各自升降——客户端没改就不动它的号,sysmodule 改了也一样,不要求两者相等。各自的查询入口:客户端自己的用 `python3 client/sysagent.py --version`(单行、不需要主机),主机上跑的用 `utility version`,并排看用顶层 `version`(两行都带标签;主机连不上时客户端那行照打、错误走 stderr、退出码 2)。改动客户端命令或文档时要一并升 `CLIENT_VERSION`(Patch 级即可),sysmodule 仍按工作区规则升。

## 协议约定

- 字节载荷只接受 hex 字节对(可选 `0x` 前缀;奇数长度或非 hex 字符拒绝,错误码 `INVALID_HEX_PAYLOAD`)。
- 普通数字是 `0x`/`0X` hex 或十进制;非法字符串解析为 0,而不是静默部分值(`12AB` 不是 hex — 写 `0x12AB`)。
- 未知按钮 token 不发任何按钮,绝不发默认键。
- `clickSeq` 支持重复语法 `ITEM*N` 与 `(ITEMS)*N`:在**命令线程**先展开成普通序列再入队
  (`source/sequence_repeat.c`,主机测试 `tests/sequence_repeat_test.c`),上限 `N≤1000`、
  嵌套 ≤4 层、展开后 ≤1024 token 与 64 KiB;展开失败一律回
  `ERR code=BAD_SEQUENCE index=<偏移> detail=<原因>` 且**不发任何按键**(不再有"静默无效"
  这种形态)。展开成功先回 `OK tokens=<n> estMs=<ms>` 再跑,跑完回 `done`;`estMs` 是时长
  估计(W 等待 + 每次 click 的 `buttonClickSleepTime`),供客户端调整等待超时。
  `clickSeqCapabilities` 报告本构建支持的能力,客户端据此在给老构建发重复语法前报错。
- `clickSeq` 序列由 `source/sequence_token.c` 按单个 `,` 切分(主机回归测试
  `tests/sequence_token_test.c`),切分器不持有静态状态:序列跑在 click 线程上,主线程
  同时在解析下一条命令行。**绝不把单字节变量的地址当 `strtok` 的分隔符串**——2.11.0 及
  更早的 `strtok(seq, &delim)` 会读到栈上未初始化字节(真机实测为 `HO_`),把 `HOME` 变成
  未知 token `ME`、`DRIGHT` 变成 `DRIG`+`T`:整条序列一个按键都不发,却照常回 `done`。
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
- **> 4 GiB 的文件**(2.13.0 起):普通文件在 FAT32 上写不过 4 GiB(真机实测:顺序写停在正好 4 GiB,`fsFileSetSize(4.5 GB)` 被拒),所以上传一律先建普通文件、越过 `0xFFFF0000` 时**按元数据转成拼接文件**(改名 → 建目录 → 文件当 `00` → `fsFsSetConcatenationFileAttribute` → 重读父目录 → 重开继续写;依据与实测见 `docs/ftp-server.md`)。4 GiB 以下的路径与改动前逐字一致;转换失败则还原文件并报 `EFBIG`,`lastFsResult` 留下失败的那一步。**已真机验证**:6,694,728,444 字节上传落成 `00`=4,294,901,760 + `01`=2,399,826,684,`SIZE` 报合并大小,整文件 SHA-256 与源一致;另用 1 MiB 阈值构建单独验证了转换本身。**上传续传(`REST` 后 `STOR`)明确拒绝**(550):本 FTP 核心的 `STOR` 是"创建并截断",顺着做会静默丢掉 marker 之前的全部内容;下载续传与 `APPE` 不受影响。转换被断电打断会在卡上留下 `<名字>.bigfiletmp`;2.13.2 起同名上传遇到这个残留会先自动删掉它(只删这一个确切名字;目录删不掉——`fsFsDeleteFile` 对目录返回拒绝,真机验证过含文件的目录完好)再重试改名一次,真机已验证;只有连删除也失败时才需手工 `DELE` 该临时文件。注意这是本适配器自己的临时名:用户若手工在那个名字上放了文件,会被一并删掉。早先"自动重试让服务无响应"的结论是当时未修的监听重建所致(对照用例同样失败),已纠正。仍待验证:`0xFFFFFFFF`/`0x100000000` 两个边界值、删除拼接文件后的空间回收(≥9 GB 三分片不再列入:转换只有一次元数据搬运,分片由文件系统自己滚,两分片用例即验收用例)。回归建议只验 `SIZE` + 分片大小 + 分片接缝与头尾各读一小段——这台机器 FTP 读写实测 4–10 MB/s,整文件 6.7 GB 走一遍要半小时以上。
- 已通过真机测试:FTP CRUD、中断清理、16 MiB hash 校验传输、`REST`/`RETR` 下载续传、生命周期命令、6000 端口响应、`RNTO` 覆盖已存在文件、> 4 GiB 上传(见上一条)。冷启动重试可观测:`ftpStatus` 暴露 `listener`/`bindAttempts`/`lastBindError`,重启后实测 `bindAttempts=3` 后监听成功。主机插着电、不会息屏、局域网也不丢包——**只有传输时机器才吃力**;过去只要 `ftpsrv_loop` 回一次错误工作线程就 `ftpsrv_exit()` 重绑,而重绑会掐断所有在途传输并让端口短暂不可达(`bindAttempts` 因此累加),这正是之前"传输中途死掉 + 端口不通"的来源;2.13.1 的计数器在 2.13.2 首次给出读数:重启后几分钟、仅一次 64 KiB 上传与一次 `getVersion` 询问的情况下,`loopErrors=6`、`listenerDrops=2`、`lastLoopError=5`(EIO)——即两轮"连续三次错误"各触发一次重建,说明这类轮询错误集中在**绑定/开机窗口**,而不是集中在数据传输期间。2.13.1 起改成**先重试循环、连续 `FTP_LOOP_ERROR_LIMIT`(3)次失败才升级为重建**,并把两端分开计数:`loopErrors`(轮询报错次数)、`listenerDrops`(因此重建监听的次数)、`lastLoopError`(errno)。实测(2.13.1):空闲 11/11 有应答(55–279 ms,均值 130 ms);64 MiB 上传期间 28/28 有应答(13–251 ms,均值 100 ms),上传 14.6 s 完成;8 轮"并发上传+下载+硬掐第三个会话"共 110/110 有应答(72–210 ms),每次上传都完成;两条 250 KB/s 节流上传连跑 5 分钟,`bindAttempts` 恒为 1、`loopErrors=0`、`listenerDrops=0`。也就是说**传输期间端口保持可用是常态**;旧版本"一次错误就整锅端"的代价已消除,但那次 `bindAttempts` 累加本身在新构建下未能复现——计数器(`loopErrors`/`listenerDrops`/`lastLoopError`)就是为这类问题留的观测口。**未关闭验收**:FTP 未与活动 C-level 搜索并发测试(含 `/switch/sys-agent/search` 写保护)。验证矩阵见 `docs/ftp-server.md`。
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
- **boot unlock**(`bootUnlock`):重启后 Horizon 停在锁屏界面("A 开始"/"A 继续"),不按键就最终休眠并掉网络。worker 在 `main()` 里、所有服务就绪之后启动(**故意不抢在 settle sleep 之前**:锁屏要停留数分钟,不需要抢时间),循环"抓屏 → 分类 → 动作 → 再看",按键一律用 **HOME**:两种锁屏与主菜单上它都无副作用,还能点亮熄灭的屏幕;A 在主菜单会直接启动当前选中的游戏,所以不用。判断只靠画面:capssc 给的是 baseline JPEG,`jpeg_dc.c` 只解每 8×8 块的 DC 系数得到 160×90 亮度图,`screen_classify.c` 按"右区大面积单色=锁屏 / 左区也单色=需连按 3 次那一型"判定,细节与阈值见 `docs/lock-screen-notes.md`;连按那一型必须一次连发(抓屏+分析超过它的 ~1 秒窗口),发现 Application 在跑立即收手。默认 `mode=armed`:不落 arm 文件就什么都不按;计划表 `sdmc:/config/sys-agent/boot-unlock.ini`,一次性请求文件 `sdmc:/config/sys-agent/boot-unlock.armed`;报告与 `sdmc:/config/sys-agent/boot-unlock.log` 记录 `reason`、轮数、按键数与逐次判定,时间字段是"开机以来毫秒"(与 `systemInfo` 的 `uptimeMs` 同一时钟)。

## 文档导航

| 路径 | 内容/何时读 |
|---|---|
| `README.md` | 项目范围、哲学与命令概览 |
| `commands.md` | 全部可用命令参考 |
| `client/README.md` | sys-agent client 说明 |
| `third_party/README.md` | 第三方源码清单 |
| `docs/ftp-server.md` | FTP 服务与验证矩阵(改 FTP 前) |
| `docs/hdls-virtual-controller-notes.md` | HDLS 虚拟手柄失效机制、自动重建/自愈、诊断命令与系统限制(改输入或手柄代码前必读) |
| `docs/lock-screen-notes.md` | 锁屏识别规则与阈值、四种界面实测数据、fixture 与主机回归(改 `boot unlock`/屏幕分类前必读) |
| `docs/headless-launch-rights-key-notes.md` | headless 启动与 external key 机制(改 launch/keys 前必读) |
| `docs/process-memory-backend.md` | 统一内存后端设计(direct/auto) |
| `docs/trace-profiles.md` | versioned trace profile schema、动态进程/模块解析、plan/run/report 与当前限制(改 trace profile 或消费方偏移配置前必读) |
| `docs/search-a-level-design.md` | A 级精确搜索设计与部署门槛 |
| `docs/search-c-level-design.md` | C 级未知值搜索设计 |
| `docs/research/audio-control.md` | 系统音量控制研究(`aud:ctl`/`audctl`) |
| `docs/research/client-roadmap-and-edizon-gap.md` | 客户端路线图与 EdiZon-SE 能力差距 |
| `docs/research/game-lifecycle.md` | 游戏启动/关闭/暂停研究 |
| `docs/research/两代switch芯片.md` | Erista/Mariko 芯片平台说明 |
