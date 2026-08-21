# sys-agent headless 启动与 external key 调查笔记（ACNH）

> 本文是 `src/sys-agent/commands.md` 的详细补记：记录一次从现象到根因、再到
> 修复的完整逆向调查（2026-08-19 → 2026-08-21），包括所有踩过的坑和走过的
> 弯路，以及最终如何找到正确方向。目的：**下次遇到同类问题，直接照着本文的
> 结论和预防清单走，不再重新蹚一遍泥潭。**
>
> 状态：**已解决并验证**（2026-08-21）。工作区改动未提交，提交需用户确认。

---

## 1. 结论速览（TL;DR）

- 现象：`sysagent.py game launch-headless 01006F8002326000` 报
  `COMMAND_FAILED (stage=launchProgram, result=0x...D802)`；
  即使能启动，加载的也可能是 base 1.x 而不是 3.0.3。
- 表层根因：headless 直连 `pmshellLaunchProgram`，跳过 ns/am 启动前向
  fsp-srv 注册 external key 的步骤，update NCA 缺 key。
- 深层机制：fsp-srv 需要的不是恒定标题密钥 `T`，而是**每 boot 变化的封装值
  `A_boot`**（安全监视器每 boot 随机数参与封装）。es 只在真正启动游戏时才
  现算并缓存 `A_boot`，所以"手动启动一次才能 headless"不是代码 bug，是
  固件机制。
- 最终突破：es 里那张"假票"的 `title_key_block`
  （`44FDFC7D7F789693C24E5AA64112658E`）**本来就是真实密文**——只是用
  **`titlekek_0a`** 加密的（NCA 头 crypto 字段指向 0x0A），而票上的
  `master_key_revision=0x0B` 把之前所有尝试都带偏到 `titlekek_0b`。
- 最终修复：客户端从 `SDcard/switch/title.keys` 的定制行
  `rightsId = block keygen` 读出加密块 + keygen，调用 sys-agent 的
  `gameExternalKeyPrepareCommon`（→ `spl:es PrepareCommonEsTitleKey`）现算
  当前 boot 的 `A_boot` 并注册，然后 headless 启动。
  **零手动启动，重启后直接可用。**

```text
验证结果（真机，2026-08-21）：
  ok=OK action=launched ... externalKey=titlekey.block+spl
  version=34  buildId=FF1D1C05670DB6021C85B624A710B963...  （3.0.3）
```

---

## 2. 环境与设备事实

- 控制台：Switch **Mariko**，Atmosphère + emuMMC（SD 重定向），固件
  **22.1.0**，Hekate 入口 `id=Atm-Emu`；SD 根目录有 `boot.dat`、
  `payload.bin`、`emuMMC/`、`atmosphere/`。
- 主机：Intel macOS；hosts 把控制台映射为 `switch`。
- 端口：`6000` = sys-agent 命令协议（纯文本单行）；`6001` = 匿名 FTP
  （仅 SD，完整 CRUD）。
- 控制台空闲一段时间会自动休眠、网络栈随之掉线（两个端口都不应答）。
  远程作业时保持屏幕常亮；`ping switch` 是最快的存活检查。
- 密钥文件（`SDcard/switch/`，均可进 git，但 key 值不打印进对话/文档/日志）：
  - `prod.keys`：只读参考。含 `master_key_*`、`titlekek_00..15`、
    `titlekek_source`、`header_key`、`key_area_key_application_*`、
    `eticket_rsa_keypair`、**`sd_seed`**（早期误以为没有，实际有）等。
  - `title.keys`：Lockpick_RCM "Dump from EmuNAND" 导出（50 条）+ 一条定制
    启动行（见 §4.2）。文件头有完整说明。
- ACNH 关键 ID：
  - base `01006F8002326000`；update `01006F8002326800`
    （= base | 0x800）。
  - rights id `01006F8002326800000000000000000B`（末字节 0x0B）。
  - patch Program content id `E10617820DB06889E1638499478DA0DE`
    （storage=SdCard）。
  - 正确 buildId `FF1D1C05670DB6021C85B624A710B963...`（version 34 =
    3.0.3；nameLen=27，UTF-16 "集合啦！动物森友会"）。
  - base 1.x buildId `7FC1BAFF976AECA414520CB89F4616CB...`。

---

## 3. 根因（完整模型）

### 3.1 表层：headless 跳过 key 注册

- 主菜单手动启动：ns/am 在 pm 启动前，把该标题所有 contents 的 external
  key 通过 fsp-srv 607 `RegisterExternalKey` 注册进去，然后才调 pm。
- headless（`pmshellLaunchProgram`）：绕过 ns，fsp-srv 里没有 key →
  loader 读 update code NCA 时 FS 报 `NcaExternalKeyNotFound`（1004）。
- key 随应用退出被注销：手动启动→关闭后立刻 headless 依然失败。
- key 缺失时 headless 会**回退加载 base 1.x**（buildId `7FC1BAFF...`），
  所以"能启动"不等于"启动对了"。

### 3.2 深层：AccessKey 是每 boot 随机封装的

fsp-srv 里那 16 字节**不是**最终 AES 密钥，而是"封装后的 AccessKey"：

```text
T        = AES-128-ECB-decrypt(titlekek[gen], title_key_block)   # 恒定
seal_boot= AES-128-ECB-decrypt(RandomForUserWrap_boot, EsSealKeySource)
A_boot   = AES-128-ECB-encrypt(seal_boot, T)                     # 每 boot 变
slot_key = AES-128-ECB-decrypt(seal_boot, A_boot) = T            # 载入时还原
```

- `RandomForUserWrap` 是安全监视器（exosphere）**每 boot 随机生成**、SE key
  slot 锁定（`KeySlotLockFlags_AllLockKek`）的密钥，用户态不可读。
- 相关源码（本地 `src/Atmosphere-src`）：
  - `exosphere/program/source/smc/secmon_smc_aes.cpp`：
    `PrepareEsCommonTitleKeyImpl`（封装）、`LoadPreparedAesKeyImpl`（解封）、
    `DecryptWithEsCommonKey`（titlekek 推导）、`PrepareEsAesKey`。
  - `exosphere/program/source/boot/secmon_boot_setup.cpp`：
    `SetupRandomKey(RandomForUserWrap, ...)`。
  - `libraries/libstratosphere/source/fssystem/fssystem_crypto_configuration.cpp`
    `DecryptAesCtrForPreparedKey`：外部 key 走 `spl::LoadPreparedAesKey`。
  - `libraries/libstratosphere/include/stratosphere/fssrv/impl/
    fssrv_external_key_manager.hpp`：fsp-srv 的 external key 表实现。

### 3.3 为什么"key 每次重启都变"（假轮换）

- 密文（NCA 原始字节）跨重启哈希一致、buildId 一致 → `T` 恒定。
- 但 `A_boot` 因每 boot 随机 `seal_boot` 而不同 → 看起来像"key 轮换"。
- 把上一 boot 的 `A_boot` 注册进新 boot → 解封成垃圾 → RomFS 哈希校验失败
  （4075）。
- es 的 `{rights_id → access_key}` 缓存表也只在**真正调用 GetTitleKey**
  （即启动游戏）时才生成，且每 boot 重启后清空。

### 3.4 为什么"假票块"其实是真 key（最关键的坑）

- es16 读出的 common 票签名全 `0xFF`（sphaira 假票），`title_key_block` 是
  `44FDFC...` + 全零（17 个不同字节值，看起来像占位符）。
- 票上 `master_key_revision = 0x0B` → 所有早期尝试（Lockpick 导出、注册
  候选、离线解密）都按 `titlekek_0b` 走，全部失败 → 误判"块是坏的"。
- **真相**：NCA 头的 crypto 字段 `crypto_type=0x02, crypto_type2=0x0B`
  → 有效 titlekek 索引 = `max(ct, ct2) - 1 = 0x0A`。用 `titlekek_0a` 解
  立刻得到正确的 `T`，且 `T` 能离线解出 update ExeFS 的 PFS0。
- Lockpick 之所以把密文原样写进 title.keys：它同样被 mkr=0x0B 误导，解不开
  就落了密文（这是 title.keys 初始值 == block 的原因）。

---

## 4. 最终修复

### 4.1 sys-agent 命令：`gameExternalKeyPrepareCommon`

```text
gameExternalKeyPrepareCommon <rightsIdHex> <titleKeyBlockHex> <keygen>
```

流程：`spl:es` cmd 20 `PrepareCommonEsTitleKey(block, keygen)`（安全监视器
SMC 0xC3000012）→ 返回当前 boot 的 `A_boot` → fsp-srv 607 注册。

关键：SMC 内部对 keygen **减 1** 再选 titlekek，所以 ACNH 传 **11**（→
titlekek 索引 0x0A）。keygen 是**每标题固定的常量**，不是动态的；spl:es
必须有它，没有默认值。它恰好等于 rights id 末字节（0x0B），理论上可自动
推导，但显式存储更稳。

libnx 封装（`src/libnx/nx/source/services/spl.c`）：
`splEsUnwrapAesWrappedTitlekey`（cmd 20，common）与
`splEsUnwrapRsaOaepWrappedTitlekey`（cmd 18，personalized）。
Atmosphere 的 spl 模块不检查调用者，sys-agent（NPDM service `*`）可直接调。

### 4.2 客户端与 title.keys 定制格式

`SDcard/switch/title.keys` 一行两种格式：

```text
<rights id hex> = <16 字节解密后的 titlekey hex>          # 标准格式（回退用）
<rights id hex> = <16 字节 title_key_block hex> <keygen>  # 定制格式（启动用）
```

ACNH 启动行（文件头有注释说明）：

```text
01006f8002326800000000000000000b = 44fdfc7d7f789693c24e5aa64112658e 11
```

`game_launch_headless_auto` 优先级：

1. title.keys 定制行（block + keygen）→ `gameExternalKeyPrepareCommon`
   （每 boot 现算，免手动启动）；
2. title.keys 标准行（旧 A_boot，重启后失效）→ 直接注册（回退）；
3. `--keys` 票 + titlekek 路径（对 ACNH 无效，仅兼容）；
4. 裸启动。

客户端解析：`load_titlekey_block_from_blocks()` 先看两段式（block+keygen），
一段式返回 None 交给旧路径；`load_titlekey_from_titlekeys()` 只处理标准行。

### 4.3 验证结果与验收标准

- `game status`：`version=34`、`buildId=FF1D1C05...` = 成功（3.0.3）。
- `buildId=7FC1BAFF...` = 又加载 base（key 没生效）。
- launch 报 `desc 1004`（`0x...D802`）= key 缺失；
  `desc 6452`（`0x...6802`）= 已注册但 key 与既有条目不一致；
  `desc 4075`（`0x...D602`）= 注册的 key 解出垃圾（RomFS 哈希校验失败）。
- 关键对照：同一 boot 内，`gameExternalKeyPrepareCommon` 的输出与
  `gameExternalKeyScan <rid> es` 读到的 es 缓存值**逐字节相同**，证明推导
  与 es 完全一致。
- 仅需注册 update ...0B 一个 key；DLC rights id（73E8/712D）启动时不需要。

---

## 5. 关键技术事实备忘

### 5.1 MAKERESULT 解码

```text
module = value & 0x1FF
desc   = (value >> 9) & 0x1FFF
```

- module 1 = Kernel（`svc_results.hpp`，desc 123 = **SessionClosed**）。
- FS module 2：desc 1004 = `NcaExternalKeyNotFound`；6452 =
  `NcaExternalKeyInconsistent`；4075 = `RomHierarchicalSha256HashVerificationFailed`。
- `0xF601` = Kernel SessionClosed：Nintendo 服务对不允许/布局错误的命令直接
  杀会话，不是礼貌的拒绝码。
- `0xE02` = FS `TargetLocked`：`fsOpen_SystemSaveData` 开 es system save
  被拒（sphaira 用 BIS 原始分区绕过）。
- 错误码裸值高 22+ 位是垃圾位，所以同一个错误每次显示不同
  （如 `0xDFC7D802` / `0xE0C7D802`）。

### 5.2 es 命令访问矩阵（FW 22.1.0 实测）

```text
可用：9/10 CountCommon/PersonalizedTicket；11/12 List...RightsIds；
      14/16 GetCommonTicketSize/Data（common 票）
0xF601（SessionClosed）：8（旧 GetTitleKey）、13、30/31、34/35
      （GetEncryptedTicketSize/Data）、501/502/503（GetTitleKey）
```

注意：**es 15/17（GetPersonalizedTicketSize/Data）只存在于 2.0.0–4.1.0**，
现代固件调它们返回 0xF601 是正常的，不能当作"权限白名单"证据。现代相关入口
就是 14/16（common）、34/35（encrypted）、503（GetTitleKey，特权）。

### 5.3 票结构（RSA-2048，704 字节 common）

```text
SignatureBlock 0x140：sig_type BE @0x00（0x10001 RSA2048-SHA1 /
  0x10004 RSA2048-SHA256）、sig @0x04
TicketData @0x140：issuer[0x40]、title_key_block[0x100] @0x180
  （common：前 0x10 即加密 titlekey）、format_version @0x280、
  title_key_type @0x281（0=common/1=personalized）、version @0x282、
  license_type @0x284、master_key_revision @0x285、rights_id @0x2A0
```

ACNH ...0B 实测：issuer `Root-CA00000003-XS00000020`、mkr 0x0B、
`title_key_block=44FDFC...`（"假票"）。

### 5.4 fsp-srv external key 表 / es access-key 缓存

- fsp-srv：`ExternalKeyEntry { prev@0x00, next@0x08, rights_id@0x10,
  access_key@0x20 }`（IntrusiveListBaseNode 在前）。扫内存找 rights id，
  pre 是两个堆指针、post 紧跟 0x10 高熵字节的才是真条目；**匹配顺序随堆布局
  变化**，不能按"第几个匹配"取。
- es：`{rights_id → access_key}` 缓存表，条目结构前缀 `02 00 00 00...`，
  rights id 后紧跟 0x10 access key，再紧跟下一项 rights id + key。
  **每 boot 惰性生成**（真启动游戏才出现），关游戏后仍保留到本 boot 结束。
- fsp-srv 607 in = `rights_id[0x10] + access_key[0x10]` inline；617 in =
  rights id。607 在"已存在同 rights id 但 key 不同"时返回
  `NcaExternalKeyInconsistent` 且不修改表。

### 5.5 spl:es 命令（现代固件）

```text
cmd 18 PrepareEsTitleKey(out, base, mod, label_digest, generation)  # personalized
cmd 20 PrepareCommonEsTitleKey(out, key_source[0x10], generation)   # common
cmd 32 LoadPreparedAesKey(keyslot, access_key)
```

服务名 `spl:es`；libnx 有现成封装（`splEsUnwrapAesWrappedTitlekey` /
`splEsUnwrapRsaOaepWrappedTitlekey`）。SMC 对 generation 减 1。

### 5.6 NCA 头 / 离线解密配方

**头部**（Python + `cryptography`）：读前 0xC00，AES-XTS 每 0x200 扇区解，
key = `header_key`（prod.keys，32 字节），tweak = 大端扇区号（从 0 起）。
解后：magic @0x200 = `NCA3`，crypto 字段 @0x206/0x220，rights_id @0x230，
`FsInfo` @0x240（`{start_sector u32, end_sector u32, hash_sectors u32, res}`，
字节偏移 = sector << 9），fs_headers @0x400（0x200 各一：
version u16@0、fs_type@2、hash_type@3、encryption_type@4、upper_iv@0x140，
BKTR patch_info @0x100）。

**正文 AES-CTR**（已验证的控制组配方）：

```text
key     = 外部 access key；或非 rights-id NCA：
          AES-ECB-decrypt(key_area_key_application_XX, key_area[0x20:0x30])
counter = int.from_bytes(fs_header.aes_ctr_upper_iv[::-1],'big').to_bytes(8,'big')
          ++ (data_file_offset // 0x10).to_bytes(8,'big')
data_file_offset = section_start + hash_region_data_offset
          （HierarchicalSha256Data region[layer_count-1].offset）
```

控制组：base NCA（无 rights id，content `80cbc793...`）ExeFS 窗口用
`key_area_key_application_09` 解出 PFS0 + `main.npdm`。

**hactool 局限**：ACNH update NCA 报 `Invalid BKTR layout!` 是工具布局检查
问题，不是 key 错；不能拿它离线验证 titlekey。

### 5.7 NAX0 容器与 SD 布局

- emuMMC SD 重定向：`emummc.ini` 的 `nintendo_path=emuMMC/SD00/Nintendo`。
- 已注册内容文件在真实 SD 上位于
  `emuMMC/SD00/Nintendo/Contents/registered/<sha256(content_id)[0]:08X>/
  <content_id>.nca`（目录名 = content id 的 SHA-256 首字节，8 位大写 hex，
  **不是** title id 派生的）。
- 这些文件**在真实 SD 上是 NAX0 整文件加密容器**（原始字节 offset 0x20 可见
  `NAX0` magic；FTP 读到的是容器字节，与系统视图不同）。系统侧（emuMMC SD
  重定向层）透明解密，`gameNcaDump` 读到的是解密后的普通 NCA。
- 离线解 NAX0 需要 sdseed + 路径；`sd_seed` 其实在 prod.keys 里，但调查中
  用系统视图（`gameNcaDump`）即可，没走离线 NAX0 路线。

### 5.8 emuMMC System 分区 FAT 与 es save 位置

- BIS System 分区是 FAT32（512B/扇区、32 扇区/簇、FAT1 从扇区 32 起、
  根目录簇 2、数据区从扇区 2592 起）。`fsOpenBisStorage(System)` 可读原始
  分区（文件系统层已由 BIS key 解密，FAT 可正常解析）。
- es 的 system save 是分区上的普通文件：
  `/save/80000000000000e0/e1/e2`（e0=cert、e1=escommon、e2=espersonalized；
  实测 e1=30MB/簇 23948、e2=140MB/簇 25192）。
- **save 镜像本身是明文**（SaveFS 容器）：offset 0x0 是 16 字节 CMAC，
  **`DISF` magic 在 0x100**（0x4100 是第二份头）。早期误判"整体加密"是因为
  只看了 offset 0。
- save 内部文件（ticket.bin）要通过 SaveFS 的 FAT/remap/duplex 层解析
  （nxdumptool `save.c`，~1800 行）。sphaira 有该解析器的移植。
- 实测：e1 里 sphaira 假票是**明文**（sig `00010004` + block + rights id
  布局齐全）；真实 ...0B 票在 save 里没有明文结构（疑似 per-ticket
  volatile 加密，最终未解密——因为 block 本身已被证明是真 key，这条路作废）。

---

## 6. 调查全过程：时间线、弯路与转折

### 第一天（08-19）：定位表层根因

1. 现象：headless 报 `0xDFC7D802` → 解码 = FS `NcaExternalKeyNotFound`
   （1004），高 22+ 位垃圾位导致每次值不同。
2. 排查 `system_commands.c` 的 launchProgram：确认直连
   `pmshellLaunchProgram`，先怀疑 storage id。
3. 关键发现：headless 加载的 buildId 是 `7FC1BAFF...`（base 1.x），手动是
   `FF1D1C05...`（3.0.3）——缺 update key 时回退 base。
4. 修复 storage/redirect：自动选 updateStorage、解析 patch Program、lr
   重定向 base→patch；能启动但仍是 base → 只剩 key 问题。
5. 实测：手动启动→关闭后立刻 headless 仍失败 → key 随应用退出被注销。
6. es16 读出 704 字节"假 common 票"；注册全零/raw/所有 titlekek 候选均
   `NcaExternalKeyInconsistent` → **误判：块是坏的**（实际是注册时的
   对照物是"封装后的 A_boot"，用错 oracle；且 kek 索引也用错）。
7. hactool 离线验证失败 = 工具 BKTR 布局限制（非 key 错）；记录 NCA 头解密
   配方；辨析 crypto 字段指向 0x0A vs 票 mkr 0x0B——**当时记下了但没跟进**。
8. Lockpick_RCM 重导 title.keys：...0B 行仍等于密文块 → 又一次"佐证块是
   坏的"（其实只是 Lockpick 也被 mkr 误导）。

**弯路 1**：把"注册候选时返回 Inconsistent"当成"块是坏的"的证据。正确
理解：Inconsistent 只说明候选 ≠ 当前已注册的封装值，与块的真实性无关。

### 第二天（08-20）：内存扫描路线与假"轮换"

9. es 探测：9/10/11/12/14/16/34 可用；8/13/30/31/35/501/502/503 全部
   `0xF601`；es34 返回 720 字节（personalized 大小）→ 提出"隐藏票"假设。
10. 决策：放弃 es 票路径 → 扫 fsp-srv 内存。
11. 实现+部署 `gameExternalKeyScan` / `esTicketCtrScan` / es35 变体
    （debug SVC）；Docker 沙盒误报 daemon 挂（实际是没提权）。
12. es35 三种布局仍 SessionClosed（废弃）；es 内存可读，但 nxdumptool 假定
    的 CTR path-hash 零命中。
13. 手动启动游戏 → 扫 fsp-srv → `ExternalKeyEntry` 提取 A_boot。
14. 运行时注册候选 → OK → 确认为"真实 key"。
15. 关游戏 → 注册 → headless → buildId 正确 → **阶段性解决**。
16. 固化到 title.keys；客户端自动注册；端到端复测通过。
17. 代码清理（删纯实验探针）。
18. 重新部署后 headless 又失败：
    (a) PATH 符号链接 bug（`__file__` 是链接路径 → title.keys 找不到）；
    (b) A_boot 变了 → 旧值 4075。
19. es 崩溃机制确认：直接 607 注册的 key 不随游戏退出清理，与 es 下次手动
    启动注册的当前值冲突 → `NcaExternalKeyInconsistent`（0xA9B26802）用户
    断点 → 大气层重启（崩溃日志 es=0x33 + ns=0x1F 成对）。
20. 三次 boot 三个 A_boot → 误判"key 随重启轮换"；NCA 密文哈希跨重启一致
    又要求 T 恒定 → 自相矛盾，未解。

**弯路 2**：把每 boot 变化的 A_boot 当成"key 轮换"，并用 buildId 当唯一
验证 oracle。buildId 只证明 ExeFS 加载对了；RomFS 是否解对要用 4075 有无
来判。真正突破前，"每次重启都要手动启动一次"被当成固件机制接受了一段时间。

**弯路 3**：FTP 的截断读取会返回损坏数据（同一静态文件多次 abort 哈希都
不同），早期"密文跨 boot 变化"结论建立在坏方法上，作废。

### 08-20 深夜：离线配方验证 + AccessKey 机制定位

- 用 base NCA 做控制组，验证了 NCA 正文 AES-CTR 配方（upper IV 字节反转 +
  绝对文件偏移计数）。
- 三份候选 A_boot 离线都解不出 update ExeFS → 确认内存里的值不是最终 AES
  key。
- 读 exosphere 源码：`LoadPreparedAesKey` 用 `RandomForUserWrap`（每 boot
  随机）解封，`PrepareEsCommonTitleKey` 反向封装 → **AccessKey 模型坐实**。
- 实测确认：es 的 access-key 缓存每 boot 惰性生成；save 镜像其实明文
  （DISF @0x100）；sphaira 假票明文在 e1；真实 ...0B 票在 save 中无明文
  结构。

**弯路 4**：在"找真实票"上投入大量精力（NSP/.tik、es IPC、es 内存、BIS
save + FAT 遍历、volatile CTR key 扫描）。NSP 全是换票 re-pack、es IPC 特权
拒绝、save 里真票疑似加密——最终证明**根本不需要真实票**，因为那张"假票"
的 block 就是真材料，只是 kek 索引用错了。

### 08-21：专家反馈与最终突破

- 外部专家几条关键修正：
  - es 15/17 只存在于 2.0.0–4.1.0，现代固件 0xF601 正常；
  - save 是明文容器，DISF 在 0x100（我们只看了 offset 0）；
  - spl:es 不能从 rights id 直接算 key，必须给 ticket material；
  - 建议 hook es→spl:es cmd20 抓 KeySource，或重查 503 ABI；
  - sphaira 目前只能 dump common 票（wiki 明示）。
- 检查 DISF @0x100 → **save 是明文**，纠正"整体加密"误判。
- 尝试离线 RSA-OAEP（eticket_rsa_keypair）解假票块 → 失败（块后全是零，
  不是真 RSA 密文）。
- 扫描 e1/e2 找票 → 只有假票明文在 e1。
- **决定性测试**：用全部 16 个 titlekek 离线解假票块 → **`titlekek_0a`
  解出正确 T，且 T 解出 update ExeFS 的 PFS0 + main.npdm**。
- 用已部署的 `gameExternalKeyPrepareCommon(block, 11)` 现算 A_boot 注册 →
  headless 启动 3.0.3；重启后零手动启动再次验证通过。
- 与 es 缓存值逐字节对比一致，证明 spl 推导 == es 推导。

**复盘**：核心错误是"kek 索引"和"验证 oracle"两件事叠加。NCA 头早就写着
`max(ct,ct2)-1 = 0x0A`（第一天就记录过），但被票上的 mkr=0x0B 带偏；而
"注册候选返回 Inconsistent"这个错误 oracle 又让"块是坏的"成了反复出现的
假结论。**教训：先离线验证推导公式（用已知明文锚点，如 PFS0/magic），再上
设备注册；不要用运行时对照当唯一判据。**

---

## 7. 工具与流程经验

### 7.1 Docker 构建

```bash
docker run --rm --platform linux/amd64 \
  -v "/Users/leo/Documents/switch 金手指/src/sys-agent:/work" -w /work \
  devkitpro/devkita64:20260219 \
  bash -lc 'source /opt/devkitpro/switchvars.sh && make'
```

- 镜像 `devkitpro/devkita64:20260219`（devkitA64 r29.2-1、libnx 4.12.0-1、
  switch-tools 1.13.1-1）。产物：
  `sys-agent/sys-agent/43000000000000A6/exefs.nsp`。
- **Docker socket 需要提权**：沙盒内报 `permission denied
  .../.docker/run/docker.sock` 是沙盒拦截，不是 daemon 挂了；显式
  `require_escalated` 即可（前缀 `docker run` 已批）。
- macOS 上不要原生装 devkitPro，只用容器。

### 7.2 部署与回滚

```bash
cd src/sys-agent
# 部署前先备份当前装机固件（FTP，需提权）：
curl --silent --show-error --max-time 60 \
  --output ../backups/sys-agent/sysagent_before_<tag>.nsp \
  "ftp://switch:6001/atmosphere/contents/43000000000000A6/exefs.nsp"
# 构建 + 上传 + 原子改名 + 重启 emuMMC：
python3 deploy_sysagent.py
```

- 部署 = 重启控制台（游戏会关），动手前先告诉用户。
- 备份写到 workspace 持久路径（如 `backups/sys-agent/`，不进 git）；
  **不要依赖 `/private/tmp`**（macOS 重启会清空）。
- 回滚 = `git checkout 对应提交` → 重新构建 → 部署；不依赖二进制备份。

### 7.3 客户端 / 协议

- 默认 timeout 10s；慢命令用 `--timeout 120`。
- 一条命令一个连接，客户端只读一行响应；多行诊断用 Python socket 直连 6000
  发送后 `recv` 短超时循环读尽（别依赖临时脚本文件）。
- 慢/危险命令顺序：先轻量探针，再重活；命令之间留间隔。

### 7.4 沙盒与提权（Codex 环境）

- workspace 读写免提权；**连 switch 的 socket / Docker / 本地绑定测试都要
  `require_escalated`**。
- 已批前缀：`docker run`、`python3 src/sys-agent/client/sysagent.py`、
  `python3 src/sys-agent/deploy_sysagent.py`。

### 7.5 FTP 教训

- 完整下载可靠（exefs.nsp 哈希一致）；**截断/abort 读取不可靠**（同文件多次
  不同哈希）。字节级对比用 `gameNcaDump`（系统视图，可靠）。
- 远程大文件对比优先用设备端扫描命令（`gameBisScan`），不要逐块下载。

### 7.6 参考源码树（先本地查，别上网）

- `src/Atmosphere-src`：fs external key manager、svc_results、NCA 加密路径、
  exosphere 安全监视器（封装/解封公式）。
- `src/nxdumptool`：`source/core/tik.c`（票结构、volatile ticket）、
  `source/core/save.c`（SaveFS 解析）、`source/core/mem.c`（debug handle）。
- `src/sphaira`：`sphaira/source/yati/nx/es.cpp`（es 命令、BIS save 读取、
  PatchTicket）、`utils/devoptab_fatfs.cpp`（BIS FAT mount）。
- `src/yuzu`：`src/core/hle/service/es/es.cpp`（es 命令表）。
- `src/hactool`：已下载；容器里 `/opt/devkitpro/tools/bin/hactool`。

---

## 8. 预防清单（下次遇到类似问题）

1. **先离线验证密钥推导，再上设备**：
   - 用已知明文锚点（ExeFS 的 `PFS0`/`main.npdm`、RomFS 的 IVFC）做"解出来
     对不对"的判据，不要只靠注册/启动结果。
   - titlekek 索引以 **NCA 头 crypto 字段 `max(ct,ct2)-1`** 为准，别被票上
     `master_key_revision` 带偏（ACNH 就是 0x0A vs 0x0B 的坑）。
2. **区分"key"与"AccessKey"**：fsp-srv/es 里见到的 16 字节是每 boot 封装的
   AccessKey；恒定 T 不能直接注册。注册必须走 `spl:es PrepareCommonEsTitleKey`
   （或等价的 es GetTitleKey）。
3. **验证 oracle 要对**：
   - buildId 只证明 ExeFS 对，RomFS 对不对要看 4075 有无；
   - 运行时注册返回 Inconsistent ≠ 材料是坏的；
   - FTP 截断读取会损坏数据，别用来比字节。
4. **先查本地源码再上网**：`src/Atmosphere-src`、`src/nxdumptool`、
   `src/sphaira`、`src/yuzu`、`src/libnx` 基本覆盖了需要的答案。
5. **es 命令号会随固件变**：查 yuzu/switchbrew 的命令表确认版本范围
   （如 es 15/17 是 4.1.0 前的东西）。
6. **SaveFS 的 DISF magic 在 0x100**（0x0 是 CMAC），别只看文件头前几字节。
7. **优先用设备端工具**：进程内存 `gameExternalKeyScan`/`gameMemDump`、
   文件/分区 `gameNcaDump`/`gameBisDump`/`gameBisScan`，比逐块下载可靠。
8. **部署/重启前备份并告知用户**；密钥值不进对话/文档。

---

## 9. 其他标题缺 external key 的恢复流程

对新的 titles：

1. 从该 title 的 common 票拿到 `title_key_block`（es16 或 NSP 内 .tik）。
2. 离线用**全部 titlekek** 试解，并用该 title NCA 的 ExeFS PFS0 窗口做锚点，
   确定正确的 kek 索引 → 得到 `keygen = kek索引 + 1`。
3. 把 `rightsId = block keygen` 写进 `SDcard/switch/title.keys`（定制格式）。
4. `sysagent.py game launch-headless <titleId>` 即自动可用；用 buildId +
   无 4075 验收。

若块本身真有问题（离线锚点全失败），再考虑：原始 NSP/.tik、es save 提取
（volatile 加密，难）、或 es 内存/缓存动态取 A_boot（需每 boot 一次手动
启动，是旧方案的兜底）。

---

## 10. 代码改动现状与清理清单（全部未提交）

### sys-agent（src/sys-agent，独立仓库，HEAD=c8ebaaf）

- 已修改：`client/README.md`、`client/sysagent.py`、`commands.md`、
  `sys-agent/source/system_commands.c`、`tests/test_sysagent_client.py`。
- 未跟踪：`docs/headless-launch-rights-key-notes.md`（本文）。
- 客户端单元测试：38 项全部通过。

### 新增命令（已构建、部署到设备并实测）

```text
gameExternalKeyPrepareCommon  <rid> <block> <keygen>   # 修复核心，必须保留
gameExternalKeyScan           <rid> <fs|es|pid>        # 恢复/诊断，保留
gameMemDump                   <fs|es|pid> <addr> <size># 诊断，保留
gameBisDump / gameBisScan     <分区> <off> <size> [pattern]  # 诊断，保留
gameNcaDump / gameNcaProbe    # 已有，保留
gameExternalKeyProbe / gameTicketRead / gameTicketListAll  # 保留
```

### 已清理的死路命令（2026-08-21 从代码移除，重新构建零警告）

```text
gameFileDump              # BIS System /save 直接打开返回 0xD401（死路）
gameTicketProbePersonal   # es 15/17 是 4.1.0 前命令，必然 0xF601（死路）
gameExternalKeyPreparePersonal  # personalized 路径，从未使用、大概率用不到
```

> 已部署（2026-08-21）：用户重新部署并复验，设备端已同步移除这三个命令
> （`gameFileDump` 不再响应，`gameExternalKeyPrepareCommon` 仍正常）。

### 根仓库

- `docs/sys-agent-headless-launch-handoff.md`（已删除，内容并入本文）。
- `SDcard/switch/title.keys`（...0B 行 = 定制格式 block keygen + 头部注释；
  其余为 Lockpick 原始导出）。
- `SDcard/switch/prod.keys`、`title.keys` 可进 git（用户已确认）。

---

## 11. 回退路线（未使用，保留参考）

1. es35 `GetEncryptedTicketData`：三种布局均 SessionClosed，关闭；es34 虽
   报 720 字节但取不到数据，无法离线 RSA-OAEP 解 key。
2. `esTicketCtrScan`：FW22 未找到假定 path-hash 的 CTR 条目（es 内部路径串
   与 nxdumptool 假设不同，且为双斜杠 `espersonalized://ticket.bin`），未走通。
3. 用户提供原版 NSP/XCU 的 .tik 或 NAND 备份：common 票用 titlekek 本地解
   （console 无关）——最终因假票块本身是真 key 而未需要。
4. es save（BIS）+ volatile ticket CTR key 路线：投入大、多层加密、未完成；
   仅当块的离线锚点全部失败时才值得重启这条线。

---

## 12. 用户协作偏好（重要）

- 避免频繁崩溃/死机风险的操作；部署/重启前先说明。
- 设备上的操作节奏由用户掌控（手动启动/关闭游戏、开关 Docker）。
- prod.keys 与 title.keys 已加入 git，可随意更改/恢复；key 值不打印进
  对话/文档/日志。
- 提交仓库需用户明确同意。
