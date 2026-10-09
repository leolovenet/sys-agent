# HDLS 虚拟手柄：失效机制、自动恢复与系统限制

> 真机结论：ACNH 3.0.3、固件 22.1.0、Mariko 主机、底座/掌机两种形态，
> 2026-10-08 至 10-09 在真实硬件上逐项复现并验收。文中每条结论都对应现场观测到的
> 返回值或画面差分；未能复现的推测不作为结论，只写进"已实测排除的候选"一节。

本文回答三个问题：**虚拟手柄为什么会在插拔底座、插拔 Joy-Con、进出游戏时失效**；
**sys-agent 现在用什么方式保证它可用**；**哪些失效是系统限制、无法解决**。

## 结论速览

- 失效有两种形态，旧版代码只处理了第一种：
  1. HOS 直接把 HDLS 虚拟设备摘掉（`hiddbgIsHdlsVirtualDeviceAttached` 返回 false），
     之后所有 `hiddbgSetHdlsState` 都写进已作废的句柄；
  2. 设备还挂着、状态写入也返回成功（`lastStateError=0`），但它已经**不被路由**，
     游戏收不到任何输入——旧版就在这时静默失效，且不会自愈。
- 现在 sys-agent 的做法（全部安全，不写任何系统表）：输入命令若距上一条命令超过 0.5 秒，
  且控制器布局指纹（滑轨状态 / handheld hids 标志 / 已连接 pad 数）与设备建立时不同，
  就把虚拟设备拆掉重建（另有 5 秒硬限速），让 HOS 与游戏把它当作"新连接的手柄"重新认领；
  设备 attach 之后要等到 HOS 把它登记进玩家 1 才写第一个状态（下限 50 ms、上限 150 ms，
  实测约 50 ms），避免"重建后第一下按键被吞"。
- 真机验收：底座 + Joy-Con 插着 / 拔出 / 插回、拿起主机（掌机）/ 放回底座，五种状态下
  一条 `input click X` 都能在动森里打开背包；纯虚拟输入还能一路走完
  HOME 菜单 → 选择用户 → 标题画面 → 进岛。
- 仍然存在的**系统限制**：HOME 菜单（qlaunch）与"由谁来游玩软件?"这类系统覆盖层对虚拟
  手柄的接受不稳定（有时生效、有时被忽略），只有游戏内与"更改握法/顺序"页稳定可靠。
- **不要使用** `hiddbgApplyHdlsNpadAssignmentState` 去改 HDLS 玩家槽分配表：它在
  2026-10-08 的真机上返回过 `0x001C2ACA`、`0x0000F601`，并让 hid:dbg 会话失效、
  大气层崩溃重启。相关命令与自动路径都已删除（见"事故记录"）。

## 机制

### 设备与玩家槽

虚拟手柄由 sys-agent 自己创建，固定是蓝牙 Pro Controller
（`HidDeviceType_FullKey3` + `HidNpadInterfaceType_Bluetooth`，机身白、握把黄/墨绿）。
HOS 会给它分配一个 unique pad id（形如 `0xFFFFFFFF800000NN`），并在槽位可用时把它挂到某个
npad 玩家槽；槽位不可用时它只出现在 npad16(Other)/npad32(Handheld)，`number` 为 0。

`controllerDump` 一次能拿到三层信息，排查时必须三层一起看：

| 来源 | 内容 |
|--|--|
| `hiddbgDumpHdlsStates` | 所有控制器（含真实 Joy-Con）的 handle / deviceType / npadInterfaceType / state |
| `hiddbgDumpHdlsNpadAssignmentState` | HDLS 的 `HdlsNpadAssignment` 原始表（handle + 若干未知字段） |
| `hidsysGetUniquePadsFromNpad` | 每个玩家槽（npad 0-7、Other、Handheld）当前归谁 |

两层 id 不能混用：`HdlsNpadAssignment` 索引 0 出现我们的 handle 并不等于
"hidsys 认为我们是玩家 1"，两者可以互相矛盾（现场见过索引 0 是我们、而
`hidsysGetUniquePadsFromNpad(0)` 指向别的 id）。**判断"游戏能不能收到"以游戏内画面为准**，
系统表只作参考。

### 拓扑变化会让设备失效

以下动作都会让 HOS 重新配置手柄，实测会打断虚拟手柄：

- 插入/拔出底座（掌机 ↔ 电视模式）；
- 把 Joy-Con 插上/拔下滑轨；
- 启动游戏（启动期间会重新枚举与重新绑定手柄，真机见过设备被摘掉或重建）。

打断的两种形态都在真机上抓到过：

- **被摘掉**：`controllerStatus` 里 `attached=0`，随后 `hiddbgSetHdlsState` 报错
  `0x1C24CA`（设备存在但无玩家槽时也会报同一族错误）。
- **保留但不再被路由**：`attached=1`、`lastStateError=0`、`slot=0`，画面毫无反应。
  旧版就卡在这种状态里：句柄没坏、写入成功、游戏收不到。

### 为什么"重建一次"就能恢复

真机对照：处于"保留但不再被路由"状态时，手动执行 `input detach-controller` 再发
`input click X`，背包立刻打开（设备换成新句柄）。也就是说 HOS 与游戏都会认领
**新出现**的虚拟设备，而不会重新认领一个"熬过拓扑变化"的旧设备。

这条性质就是现在自动恢复的基础，且完全不需要写系统表。

### 重建后必须等一拍

在同一个命令里"先重建、紧接着写按键"会被吞：真机上表现为"带重建的第一下 `click A`
无效、紧跟其后的第二下 `click A` 生效"。原因是设备刚 attach 时 HOS 与游戏还没纳管它，
50 ms 的按下+抬起都发生在纳管之前。现在 rebuild 后等到"玩家 1 已登记我们的 pad"再写第一个
状态（下限 50 ms、上限 150 ms），真机复测第一下就生效，且实测只用 ~52 ms。

抢占**不需要**替玩家确认系统弹出的"请按 L+R / A"页：真机实测断开占位手柄后直接发输入即可，
按键照常到达游戏；那套"等页面出现再补一个短 A"的逻辑（200 ms 窗口、50 ms 按住、只在有应用
运行时才补）已删除，每次抢占因此少约 250 ms。

## 现在实现的行为

| 触发 | 动作 | 目的 |
|--|--|--|
| 每条 `click` / `press` / `release` / `setStick` / `clickSeq` 命令开始，且距上一条输入命令 ≥ 0.5 s | 若布局指纹（滑轨状态 / handheld hids 标志 / 已连接 pad 数）与设备建立时不同，就拆掉重建（`controllerRefreshIfIdle`，另有 5 s 硬限速） | 覆盖插拔底座、Joy-Con 插拔、进出游戏等发生在"空闲期"的拓扑变化，同时避免无意义的设备churn |
| 距最后一条输入命令 ≥ `controllerIdleRelease`/`controllerIdleReleaseMs`（默认 1000 ms，0 = 不释放）**且存在非 handheld 手柄（或抢占时踢掉过一只）**，同时状态中性、没有 `clickSeq` 在跑 | 摘掉虚拟设备（`controllerServiceIdle`，在主循环内执行） | 把玩家 1 让回主机自己的手柄；没有可让位的手柄时保持挂载，避免"无手柄空窗期"让游戏弹窗 |
| 每条输入命令 | 检查 `hiddbgIsHdlsVirtualDeviceAttached`，不成立就拆除重挂（`ensureControllerLocked`） | 设备被 HOS 摘掉后立即自愈 |
| 设备 attach 之后 | 等到玩家 1 已登记我们的 pad 再写第一个状态（`controllerWaitForAttachLocked`：下限 50 ms、上限 150 ms，实测 ~52 ms） | 避免重建后第一下按键被吞 |
| 设备挂载时玩家 1 已被真手柄占着 | 按 `controllerTakeover` 处理（默认 1：断开占位者 → 重建 → 写状态） | 让自动化在被真手柄占着时也能拿到控制权 |
| `hiddbgSetHdlsState` 失败 | 再按 `controllerTakeover` 抢一次槽（断开占位者）→ 重建 → 重试一次；仍失败则打印一行 `ERR controllerState result=0x… attached=… takeover=…`（同一错误只打印一次） | 兜住"挂载成功但没被路由"的情况，不再静默丢输入 |
| 重建时 | 保留调用方当前意图的按键/摇杆状态；只有显式 `detachController` 才清零 | 长按/推杆不会被后台重建打断 |
| 长时间 `clickSeq`（含 `W` 等待）运行中 | 每个 token 之后刷新"最后输入时间" | 序列跑到一半不会被空闲释放打断 |

阈值取舍：拓扑变化只可能发生在没人发命令的时候，所以 0.5 s 的空闲判定不会误伤连发脚本；
代价是**设备被让位或重建之后**的第一条命令要多一次 attach（实测 ~52 ms 稳定等待）。
5 s 硬限速则保证即使布局频繁抖动，也不会出现无意义的设备 churn。

## 仲裁：谁控制角色（底座模式下无法两全）

真机验证（底座模式、动森在岛上）：

- 虚拟手柄占着玩家 1 时，真 Joy-Con 即使按 L+R 也**完全不能控制角色**；
- 把虚拟手柄彻底摘掉后，动森立刻弹出"手柄连接请求"，用真 Joy-Con 按 L+R 接受后，
  X 开背包、B 关背包全部正常。

也就是说：**ACNH 把一名居民玩家绑定到一只手柄上**，谁占着玩家 1（那个 1 号灯）谁就控制角色；
修复前"真手柄正常"的代价正是虚拟手柄当时根本没被路由。需要说明的是，真机两种时序都出现过：
先由虚拟手柄占住玩家 1、真手柄后连时，有过"两只都能驱动游戏"的情形；也有过真手柄被分到
玩家 2、在单人游戏里失去控制的情形（见"早期占位（B1）"一节的实测数据）。**因此共存不能当作
可依赖的前提**，下面按"可能被锁在门外"来设计。

因此 sys-agent 采用**统一策略：把虚拟手柄当作"补充的、用完可扔的"输入设备**，
底座与掌机共用同一套规则：

| 场景 | 行为 |
|--|--|
| 主机上**没有非 handheld 手柄**（Joy-Con 只插在滑轨上，没有拔下来；也没有第三方手柄） | 虚拟手柄就是唯一的外部蓝牙手柄：使用后**保持挂载，不自动让位**——游戏始终有手柄可用，不会弹"请连接手柄" |
| 主机上**存在非 handheld 手柄**（拔下来的 Joy-Con / 第三方蓝牙手柄） | 虚拟手柄照常可用，**用完后立即让位**把玩家 1 交回那只手柄；若使用期间玩家 1 被那只手柄占着（我们的状态写入会被拒），则按 `controllerTakeover` 抢占：断开它 → 重建 → 补一个短 A，用完再按蓝牙地址把它拉回（见下节） |
| 让位之后 | 那只手柄本来就是连接的，继续用即可；动森若弹"手柄连接请求"，用真手柄按 L+R 接受——这一步只能人工完成 |

判断"有没有非 handheld 手柄"用的是 `hidsysGetUniquePadIds` 减去我们自己的 pad、再减去
`hidsysIsJoyConAttachedOnAllRail` 报出的两只滑轨 Joy-Con（滑轨 Joy-Con 在底座模式下不能驱动
游戏，让位给它们等于让游戏没人可用）。另外，**如果抢占踢掉过一只手柄，它会被记下来**：即使
它暂时从 pad 列表消失，用完也必须让位，否则它会永远连不回来。

`configure controllerIdleRelease <seconds>`（默认 1 s）就是"用完"的判定窗口，设 0 表示永不释放
（虚拟手柄一旦用过就常驻玩家 1）。让位还要求当前虚拟状态是"中性"的（没有按键被按住、摇杆
回中、没有正在跑的 `clickSeq`），所以长按、推杆和多段序列不会被中途打断。
这个窗口就是"真手柄下线"的时长里唯一由我们掌握的一段（让位之后的两条蓝牙链路约 2 s，见下节），
所以 `configure controllerIdleReleaseMs <milliseconds>` 可以按毫秒设：单次动作想要尽快交还就调小
（实测 300 ms），连续命令的脚本反而要调大（命令间隔大于该窗口时，每段间隔都会多花一次"抢占 +
两条链路"）。

### 抢占 + 自动拉回（默认行为）

真机确认过的现象：

- **真手柄先占住玩家 1 时，新挂上的虚拟设备收不到**：`hiddbgSetHdlsState` 返回 `0x1C24CA`，
  重建+重试也拿不回来；
- 另一种时序（虚拟手柄先占住玩家 1、真手柄后连）两次测量结果不同：一次两只都能驱动游戏，
  一次真手柄被分到玩家 2 而失去控制权（后者见"早期占位（B1）"的实测数据）。因此**不能假定
  共存会发生**；
- 需要"抢"的时候，唯一可用的办法是 `hidsysDisconnectUniquePad` 断开占位手柄。

被断开的手柄**仍是配对状态**（`btdrvGetPairedDeviceInfo` 返回 0），但它**自己不会再回连**：
按键、甚至按同步键都无效，必须滑回滑轨才会恢复。真机实验发现主机可以主动把它拉回来：

1. 断开前用 `hidsysGetUniquePadBluetoothAddress` 记下每只被断开手柄的蓝牙地址；
2. 用完让位后，主循环用 **`btdrvTriggerConnection(addr, 5000)`** 主动触发回连；
   适配器一次只寻呼一条链路：同一对 Joy-Con 的第一只被接受后，第二只直到第一只的链路
   建立完成为止一律返回"链路忙"（`0x002F4471`），真机实测这个窗口是 **0.5–1.3 s**
   （第一只自己建立链路也要约 1 s）。**窗口内没被接受的寻呼不会自动补发**，手柄会一直
   离线，所以重试必须覆盖整个窗口：失败地址按主循环节拍 **50 ms** 连打（预算 40 次 ≈ 2 s），
   之后留 0.25 s / 0.6 s 两次慢速兜底，总窗口约 2.9 s；寻呼一旦被接受该地址立即出列，
   不再打扰已经回来的手柄。真机实测（发一条 `click X`，以让位时刻为 0）：第二只的寻呼在三轮
   测试里分别在第 **0.49 / 0.90 / 0.93 s** 被接受，第一只约 **0.6–1.1 s** 回到 hidsys，
   两只齐（`uniquePads=2`、都在 npad0）约 **1.7–2.8 s**：剩下那段是两条蓝牙链路自己的
   建立时间，改不动。**回连只在让位之后触发**：占着玩家槽时发出的寻呼会被接受却永远不注册
   （见"已实测排除的候选"），提前拉反而会把重试预算用光、把手柄留在离线状态。

于是 `configure controllerTakeover` 默认 **1**：断开占位手柄 → 重建虚拟设备 → 写状态 →
用完让位并自动把真手柄拉回。`0` 是"完全不碰真手柄，只把写失败如实报出来
（`ERR controllerState …`，同一错误只报一次）"；沿用旧脚本写的 `2` 与 `1` 行为相同
（"补 A"已删除）。

残留的人工步骤：真手柄被拉回后，动森可能再弹一次"手柄连接请求"，需要玩家按真手柄的
**L+R** 接受——这一步 sys-agent 无法代按（被抢走后玩家自己也可能需要重新选中手柄）。

## 已实测排除的候选（2026-10-09）

这些候选的目标都是"不抢所有权也能注入"。结论是它们在 22.1.0 上都不成立；对应的探针命令
已在整理时删除，这里只保留结论，避免再走一遍弯路。

| 候选 | 实测结果 | 结论 |
|--|--|--|
| HDLS `npadInterfaceType = Rail` 的 merge 语义 | 把虚拟设备挂成 `JoyRight1 + Rail`，真 Joy-Con 走蓝牙时**不合并**：我们的 pad 只出现在 npad16(Other)/npad32(Handheld)、`number=0`，npad0 仍是真手柄；游戏无反应 | merge 只在真实 Joy-Con 也在滑轨（iface=Rail）时成立，对"分离 Joy-Con"场景不可用 |
| `hidsys ActivateUniquePad`（命令 700） | 挂载后对我们的 pad（`FFFFFFFF8000000A`）调用 → `rc=0x0000F601`（服务关闭会话），前后 npad0 完全不变 | 从 sysmodule 不可用 |
| `hidsys SendConnectionTrigger`（545/1156） | 同样返回 `0xF601` 并关掉会话（会话自愈后恢复） | 回连只能走 `btdrvTriggerConnection` |
| `hiddbgSetDebugPadAutoPilotState`（DebugPad autopilot） | 调用成功（`setRc=0x0`，attributes=IsConnected，X 按住 300ms），但动森画面毫无反应 | 游戏只读 npad 表，不读 DebugPad 通道；不是可用通道 |
| 让位前就提前触发回连（虚拟设备仍占着玩家 1 时先寻呼真手柄，好让两条蓝牙链路与让位窗口重叠） | 8 s 安全网期间连发两轮：`trigger[0]` 立即被接受、`trigger[1]` 0.6 s 后也被接受，但之后 4.7 s 内 `uniquePads` 始终只有我们自己；让位后地址已全部出列、不再重试，两只真手柄一直离线（要靠 `controllerReconnect <addr>` 手工救回）。期间我们自己 pad 的 hidsys 条目还消失了两次又在 0.7 s 后回来 | **"寻呼被接受" ≠ "手柄已注册"**：占槽时触发既不生效又白耗重试预算，所以现在只要虚拟设备还占着玩家槽就完全不触发回连（`idleRelease 0` 时真手柄就一直离线，直到让位）——"让位 → 再拉"是必须顺序，不是保守 |

顺带记录一个容易误判的现象：以默认 `FullKey3 + Bluetooth` 挂载、而 npad0 已被真手柄占用时，
HOS **根本不把我们的设备登记成 pad**（`hidsysGetUniquePadIds` 数量不变），紧接着的状态写入
返回 `0x1C24CA`、设备被摘掉（`attached=0`）。也就是说这种时序下连"挂着但收不到"都算不上，
是直接被拒。

因此就 sys-agent（系统级、游戏无关）而言，**抢占 + 抢占后回连是目前唯一可用的完整路径**；
若要彻底零中断，只能走游戏进程内注入（例如 acnh-agent 每帧把按键并进游戏读的 HID 共享内存），
那属于各游戏自己的实现，不是 sys-agent 的通用能力。

## 早期占位（B1）：实测判负，代码已删除

动机：抢占会被触发是因为我们"晚了一步"；那就让我们早点到——启动时先静默占住玩家 1，
期望真手柄之后连上时会**追加进同一个 npad0**从而实现共存。

真机实测（2026-10-09，底座模式）：启动时预占位成功（`initialised=1`、`npad0` = 我们、`number=1`），
随后把两只已配对的 Joy-Con 拔下来（`linkKey=1`，配对未丢）：

```
我们的设备 : npad0, number=1     ← 玩家 1
Joy-Con L/R: npad1, number=2     ← 被 HOS 分到玩家 2，不是追加进 npad0
lastActiveNpad=1
```

**结论：22.1.0 上 HOS 不会把后到的控制器追加进已占用的玩家槽，而是给它下一个空闲槽。**
在单人游戏里玩家 1 才是角色控制器，所以常驻预占位等于把玩家锁在门外（用户实测：标题界面按
Joy-Con 的 A 无效）。因此预占位**没有保留**：代码与开关都已删除，只留本节结论备查。

顺带得到两个可复用的事实：

1. **配对与连接是两件事**：`controllerPairedDevices`（`setsysGetBluetoothDevicesSettings`）能列出
   已配对设备（含地址与 `linkKey`），被主机断开的手柄依然在列表里；"连不上"是主机不去连，
   而不是配对丢了。
2. **回连必须由主机发起**：`controllerReconnect <ms> [addr]` 可按地址（`AA:BB:CC:DD:EE:FF`）
   触发 `btdrvTriggerConnection`，实测能把被断开的手柄拉回来；一对 Joy-Con 的第二只常报
   "链路忙"（`0x2F4471`/`0x313871`），重试即可。

实现细节：真实手柄数量来自 `hidsysGetUniquePadIds`（减去我们自己的虚拟 pad），采样缓存在
主循环里每 0.5 s 刷新一次；`hid:sys` 会话在整个 sysmodule 生命周期内**只建立一次**
（早期版本在每次主循环 tick 里 init/exit，把服务会话打爆，命令会超时）。回连触发放在主循环
里分次执行（不 sleep 阻塞），避免拉长命令响应时间；`hid:sys` 被服务关闭时会自动重新初始化。
`bt:drv` 会话相反：每个回连 pass 都 exit/init 一次（服务拒绝请求时会自行关闭会话，重新开关
比复用更稳），所以 50 ms 节拍的重试每轮只多一次会话开销。

## 时间阀与实测耗时（2026-10-09 调优后）

| 阀值 | 现值 | 位置 / 作用 |
|--|--|--|
| 布局重建前置空闲 | 0.5 s | `controllerRefreshIfIdle`：距上一条输入命令多久才检查布局指纹 |
| 布局重建限速 | 5 s | 同上：防止布局抖动造成的设备 churn |
| attach 稳定等待 | 下限 50 ms / 上限 150 ms（实测 ~52 ms） | `controllerWaitForAttachLocked`：等到玩家 1 登记我们的 pad |
| 用完让位空闲 | 默认 1000 ms（`controllerIdleRelease` 秒或 `controllerIdleReleaseMs` 毫秒，0 = 不让位） | `controllerServiceIdle`；这是"真手柄下线"里唯一由我们掌握的时长 |
| 让位判据采样 | 0.5 s 缓存 | `controllerPadSampleCached`：有没有非 handheld 手柄 |
| 主循环节拍 | ~50 ms（poll 超时后不再额外 sleep） | 让位与回连的响应粒度 |
| 回连触发 | 让位后同一次循环内立即触发；失败地址按主循环节拍 50 ms 重试、预算 40 次（≈2 s），之后 0.25 s / 0.6 s 两次兜底（总窗口 ≈2.9 s）；`btdrvTriggerConnection` 超时 5 s | `controllerServiceReconnect` |
| 抢占期间回连抑制 | 只要虚拟设备还占着玩家槽就完全不触发回连 | 实测：占槽时发出的寻呼会被接受，但**不会注册成手柄**，还白耗重试预算（`idleRelease 0` 时表现为真手柄一直离线，直到让位） |

一次抢占（真手柄正在玩，真机实测）：

| 阶段 | 耗时 |
|--|--|
| `hidsysDisconnectUniquePad` ×2（一对 Joy-Con） | ~2 ms |
| `hiddbgAttachHdlsVirtualDevice` | <1 ms |
| 稳定等待（等到玩家 1 登记我们的 pad） | ~52 ms |
| 本次按键（`click` 的按下+抬起） | ~50 ms |

让位与回连（以命令发出为 t0，真机实测，2026-10-09 短重试版）：**t+1.1 s** 让位（`controllerIdleReleaseMs`
1000 + 主循环节拍）、**t+1.7~2.2 s** 第一只 Joy-Con 回来、**t+2.8~3.9 s** 两只齐；把让位窗口
降到 **300 ms** 后同一流程变成 **t+0.46 s** 让位、**t+1.6 s** 第一只、**t+2.5 s 两只齐**。
占时间的大头在系统侧：适配器一条一条地寻呼，第二只必须等第一只的链路建立完（"链路忙"窗口
0.5–1.3 s），两条链路各约 1 s。能压缩的只有两段：一是让位窗口（我们自己的定时器，见上），
二是"寻呼被拒绝后干等"——旧版用固定/长间隔重试，第二只最快也要等一个间隔（0.25–1.5 s）才再试
一次，现在跟着主循环节拍 50 ms 重试，被拒绝到重试之间只差一个循环。
游戏若要求人工按一次 L+R 接受"手柄连接请求"，那一步仍要玩家操作。

`clickSeq` 与单条 `click`/`press`/`release`/`setStick` 现在走同一条 `ensureControllerLocked` 路径
（2026-10-09 修复：此前 `clickSeq` 直接调 `initControllerLocked`，会先白挂一次、浪费约 155 ms，
并多报一次 `ERR controllerState`）。

## 诊断命令

| 命令 | 作用 |
|--|--|
| `controllerStatus` | 单行汇总：`initialised`、handle/session、`attached`、`deviceType`/`interface`、`idleRelease`、`takeover`、`HdlsNpadAssignment` 里命中的行号（`slot`）、`assignmentRc`、`lastStateError`、以及 hidsys 视角的玩家 1 归属（`npad0=`，`free` 表示无人持有）。**只读：设备没挂就报 `initialised=0`，不会为诊断创建手柄** |
| `controllerDump` | 多行详表：三层信息全量打印（十六进制）+ 布局指纹 + pad 数量与 interface/编号；排查拓扑问题时先抓这个。设备没挂时只打印 hidsys 部分，正好用来确认"让位/回连"是否成功 |
| `controllerPairedDevices` | 列主机已配对的蓝牙设备（地址 + `linkKey` + 名字），用来区分"配对丢失"与"主机没去连" |
| `controllerReconnect [ms] [addr]` | 对抢占时记下的地址（给了 `addr` 就把它**加进**那张表，不替换）触发 `btdrvTriggerConnection` 主动回连；自动让位走的是同一个调用 |
| `configure controllerIdleRelease <seconds>` | 空闲多少秒后把玩家 1 让回主机手柄（默认 1，0 = 永不释放） |
| `configure controllerIdleReleaseMs <milliseconds>` | 同一个窗口的毫秒写法，用来做亚秒调优（实测 300 ms 时"命令 → 两只 Joy-Con 都回来"从 3.3~4.1 s 降到 2.5 s）；连续命令的脚本要调回大值 |
| `configure controllerTakeover <0\|1>` | 被真手柄占住玩家 1 时的行为：0=不碰（只报错）、1（默认）=断开占位手柄后重建并重试写入；旧的 `2` 与 `1` 同义 |
| `controllerKick <0-7>` | 手动执行抢占第一步（断开占位手柄），用于验证"是不是这一步影响了真手柄" |
| `input detach-controller` | 手动强制重建（等同自动恢复那一步），排查时可用 |
| `configure printDebugResultCodes 1` | 打开后每一步会多打一行耗时：`ctrl kick:`（断开占位手柄）、`ctrl attach:`（重建 + 稳定等待 + pad 数）、`ctrl reconnect: pass=N pending=M t=…ms`（回连第几轮、还剩几个地址、距本轮第一次触发多少毫秒）与 `ctrl trigger[i] rc=…`（哪一地址、什么错误码）。排查"第二只 Joy-Con 没回来"时先开这个 |
| `configure`（读） | 回读当前开关值：`configure` 无参 = 全表（`name=value` 每行一条，以 `END configure` 结束），`configure <参数>` = 单行；客户端对应 `config get <参数>` / `config list`。`controllerIdleRelease` 是秒写法别名，回读统一报 `controllerIdleReleaseMs` |

客户端对应 `controller status|dump|kick|reconnect|paired`。其中 `dump|kick|reconnect|paired`
的应答是多行块，以 `END <命令>` 结尾，客户端会读完整个块（`status` 是单行）；
旧式命令（`click` 等）若出错会多打一行 `ERR controllerState …`，只读第一行的客户端看不到，
需要读完整响应。

## 真机验收矩阵（2026-10-08 至 10-09）

验收方式统一为：动森在岛上，发一条 `input click X`，抓一帧对比背包面板是否出现/消失
（过程截图在工作区已忽略的 `src/sys-agent/build/scratch/`，不入库）。默认配置为
`controllerIdleRelease 1`、虚拟手柄为蓝牙 Pro Controller，被真手柄占住玩家 1 时执行抢占。

| 状态 | 结果 |
|--|--|
| 底座 + Joy-Con 插着 | 背包打开 ✅ |
| 底座 + 拔出 Joy-Con | 背包切换 ✅（设备已被自动重建） |
| 底座 + 插回 Joy-Con | 背包打开 ✅（旧版在这一步失效） |
| 拿起主机（掌机）+ Joy-Con 插着 | 背包打开 ✅ |
| 放回底座 + Joy-Con 插着 | 背包打开 ✅ |
| 纯虚拟输入启动游戏：HOME → 选择用户 → 标题 → 进岛 | 全程成功 ✅ |

每次成功时 `controllerStatus` 都是 `attached=1`、`lastStateError=0`，且句柄在拓扑变化后
自动变成新的（例如 `0x…0003 → …0005 → …0007 → …000A`），这正是自动重建生效的标志。

## 系统限制（无法用这套方案解决）

1. **系统覆盖层对虚拟手柄的接受不稳定**：HOME 菜单、`由谁来游玩软件?` 这类 applet
   覆盖层有时接受、有时忽略虚拟输入（同一状态下连续两次 `click A` 可以一次生效一次无效）。
   游戏内（ACNH）与"更改握法/顺序"页始终可靠。自动化不要依赖系统 UI 导航，
   需要启动游戏时用 `game launch-headless`，或先让游戏处于前台。
   **覆盖层出现时也挡回连**：反复手动触发回连（`controllerReconnect`）时，主机曾在首页弹出手柄
   提示层；提示层在的时候，被寻呼的那只手柄一直返回链路忙（`0x313871`）且始终不注册，
   点掉提示层后下一次触发就正常回来。虚拟手柄在覆盖层上不一定被接受，但**虚拟触摸可以点掉它**
   （`input touch <x> <y>`，不需要玩家槽），这是这种情况下可用的操作入口。
2. **HOS 可能保留一个不再被路由的设备**：只能用"重建"绕过（对外表现为手柄重新连接），
   无法让 HOS 把旧设备重新纳入；这属于系统侧行为，不做进一步推断。
3. **`HdlsNpadAssignment` 写入不安全**：`hiddbgApplyHdlsNpadAssignmentState` 的成功/失败
   与表内容语义都未摸清（本轮唯一确认的用法在真机上崩过），因此"用系统表把虚拟手柄
   塞进指定玩家槽"在 22.1.0 上视为不可用。

## 事故记录（2026-10-08）

为验证"显式占玩家槽能否修复"这一假设，sys-agent 曾自动在每次 attach 后调用
`hiddbgApplyHdlsNpadAssignmentState`。在"底座 + 系统选择页"状态下该调用返回
`applyRc=0x0000F601`、设备变成 `attached=0`，随后大气层崩溃、主机重启。事故后立即回滚，
并在最终整理时**彻底删除该命令**（`controllerBind` 已不存在）：自动路径里没有任何写
`HdlsNpadAssignment` 的调用，手动入口也不再提供。**结论：该 API 在 22.1.0 上不可用。**

## 复现步骤

1. 动森进岛，确认 `input click X` 能开背包（基线）；需要时先 `controllerStatus` 记一次现状；
2. 把 Joy-Con 拔下滑轨，再执行 `input click X` → 应仍能开背包（若玩家 1 被真手柄占着，
   这一次会走抢占：真手柄被断开、重建后输入生效）；`controllerStatus` 应为
   `attached=1`、`lastStateError=0`（设备被重建时句柄会换新）；
3. 把 Joy-Con 插回滑轨，`input click X` → 应仍能开背包；
4. 拿起主机（掌机）与放回底座，各执行一次 `input click X` → 均应能开背包；
5. 若某一步画面没有反应：先 `controllerStatus` / `controller dump` 记录状态，再
   `input detach-controller` 后重发一次；仍然无反应时保留 `controller dump` 输出，
   这属于本文"系统限制"一节要补的新证据。

## 相关

- 控制器命令参考：`commands.md` 的 Controller Input 一节；
- 客户端用法：`client/README.md` 的 `controller` 组；
- 游戏内按键读取（ACNH 进程内 HID 共享内存）：`src/acnh-agent/docs/acnh_in_game_hid_input_notes.md`。
