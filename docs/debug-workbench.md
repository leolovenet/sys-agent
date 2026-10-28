# 通用调试工作台

本文是新会话排查 Switch 进程、系统服务、启动期逻辑和 IPC 的首选入口。目标是快速回答：

- 哪个进程在做什么；
- 哪个函数执行了；
- 第一次失败发生在哪一侧；
- 请求和响应如何对应；
- 一次操作跨多个进程时，事件按什么顺序发生。

sys-agent 本身始终是通用工具。Title ID、module offset、函数入口/返回地址、service
名称和 Command 名称由消费方 profile 或 Ghidra 项目提供，不写进 sysmodule。

## 选择工作流

先用成本最低的手段缩小范围，再决定是否上 trace：

| 问题 | 首选工具 |
|---|---|
| 进程是否存在，PID/Title ID 是什么 | `system process-list` |
| 游戏或应用是否运行，main/heap/Build ID | `game status` |
| 某个函数是否执行 | `debug trace` 或 profile `trace-run` |
| 需要函数返回值 | entry + caller return 两个目标 |
| 请求或响应结构体在哪里 | 命中时 `--snapshot REG:SIZE` |
| 客户端和系统服务两侧同时发生什么 | `debug multitrace` / 双进程 profile |
| 启动初始化是否被错过 | `trace-run --wait-process` + `base=code` |
| HIPC/CMIF 消息是什么 | `trace-ipc` / `ipc-decode` |
| 多次 trace 的先后关系 | `trace-timeline` |

## 最短工作流

### 1. 看进程

```bash
python3 client/sysagent.py --host switch game status --json
python3 client/sysagent.py --host switch system process-list --count 64
python3 client/sysagent.py --host switch debug read PID main+0x1234 16
```

没有专用客户端子命令的通用检查命令可通过 `raw` 调用：

```bash
python3 client/sysagent.py --host switch raw debug modules PID 2
python3 client/sysagent.py --host switch raw debug maps PID 64
python3 client/sysagent.py --host switch raw debug threads PID
```

### 2. 直接 trace 一个已运行进程

```bash
python3 client/sysagent.py --host switch debug trace \
  main+0x123456 main+0x123460 \
  --pid 0x71 --ring 64 --duration 30 \
  --snapshot x1:0x100

python3 client/sysagent.py --host switch debug trace-status
python3 client/sysagent.py --host switch debug trace-dump --output trace.jsonl
```

短操作来不及由人触发时，可让 duration 从首个命中开始：

```bash
python3 client/sysagent.py --host switch debug trace \
  main+0x123456 --ring 64 --duration 30 --duration-from first-hit
```

### 3. 用 profile 固化一次调查

Profile 让命令可重复，并校验 Title ID、Build ID、指令字节和地址。最小的单进程 profile：

```json
{
  "schemaVersion": 2,
  "name": "example-entry-return",
  "symbols": {
    "entry": "0x1000",
    "return": "0x1010"
  },
  "processes": [
    {
      "id": "target",
      "foreground": true,
      "expect": {
        "titleId": "0100000000000000",
        "buildId": "BUILD_ID_PREFIX"
      }
    }
  ],
  "targets": [
    {
      "id": "entry",
      "process": "target",
      "symbol": "entry",
      "expectBytes": "FD7BBFA9",
      "snapshots": [
        {"spec": "x1:0x100", "name": "request", "kind": "ipc"}
      ]
    },
    {
      "id": "return",
      "process": "target",
      "symbol": "return"
    }
  ],
  "pairs": [
    {"entry": "entry", "return": "return"}
  ],
  "trace": {
    "hits": 16,
    "ring": 16,
    "duration": 30,
    "durationFrom": "first-hit",
    "args": 8
  }
}
```

常用流程：

```bash
python3 client/sysagent.py --host switch debug trace-plan profile.json
python3 client/sysagent.py --host switch debug trace-run profile.json \
  --output trace.jsonl
python3 client/sysagent.py debug trace-report trace.jsonl profile.json --json
```

完整 schema、双进程布局、快照和 error 规则见
[trace-profiles.md](trace-profiles.md)。

### 4. 从第一条指令开始

PM 的 create-process hook 会把目标停在主线程启动前。`trace-run --wait-process`
会自动完成“等待 → arm → 确认 armed → StartProcess → 收事件”：

```bash
python3 client/sysagent.py --host switch debug trace-run startup-profile.json \
  --wait-process --wait-process-timeout 60000 --output startup.jsonl
```

启动入口不是 main 模块基址，而是整个 process code region 的基址。profile target 写：

```json
{
  "id": "process-entry",
  "process": "target",
  "base": "code",
  "offset": "0x0"
}
```

`base` 默认是 `main`。`base=code, offset=0x0` 才对应 kernel `StartProcess` 的入口，
不需要写死 ASLR 地址。

sysmodule 的等待本身是后台会话，不阻塞输入、截图或状态查询。客户端
`wait-application` / `wait-title` 会替你轮询到 `parked` 或 `exists`：

```bash
python3 client/sysagent.py --host switch debug wait-application 60000
python3 client/sysagent.py --host switch debug start-process PID
```

`wait-status=parked` 表示进程仍被暂停。必须 `start-process` 才继续执行。
如果要在另一个终端观察底层异步会话，使用 raw wire 命令：

```bash
python3 client/sysagent.py --host switch raw debug wait-title TITLE_ID 60000
python3 client/sysagent.py --host switch debug wait-status
python3 client/sysagent.py --host switch debug start-process PID
```

### 5. 双进程和 IPC

用双进程 profile 同时记录 client/server 入口和返回点，并在服务入口/返回点保存 HIPC
缓冲区快照。典型 target snapshot 是 `x1:0x100`。导出后：

```bash
python3 client/sysagent.py debug trace-ipc trace.jsonl \
  --profile profile.json --json

python3 client/sysagent.py debug trace-timeline trace.jsonl \
  --profile profile.json --json
```

重点看：

- `commandId`、Command ID 名称和 `service`；
- response `Result`；
- request/return thread；
- `correlation` 和 `correlationAmbiguous`；
- `interfaceMatchesProfile`；
- `requestHandles` / `responseHandles`；
- domain object 的 create/close 和 Result 分布。

默认按 session + thread + entry/return 配对。并发调用可让 profile 提供 correlation key；
无法证明的匹配会标成 ambiguous，不要把 ambiguous 当成确定结论。

## 跨进程因果排查顺序

排查错误时不要从最终 Error applet 倒猜。推荐顺序：

1. 固定错误身份和出现时间；
2. 画出 game / middleware / system service / worker / Error applet 的进程边界；
3. 在客户端入口和 caller return 捕获返回值；
4. 在服务 handler 和响应点捕获 IPC；
5. 找第一个非零 Result；
6. 用 snapshot 查看对应请求/响应对象；
7. 多会话结果用同一 boot 的 `trace-timeline` 排序；
8. 结论只写已证实和已排除的内容。

`x0` 只是返回值候选。只有 entry/return、IPC Result 或已知 ABI 证明它表示 Result 时，
才能把它当 `nn::Result`。

## 安全边界

- 硬件执行断点不修改目标代码；`debug patch-code` 是另一项显式写操作。
- `hits` 和 `duration` 是安全上限，不是采样周期。热点指令会拖慢甚至触发 watchdog。
- 跨进程 multitrace 最多两个进程、每进程两个目标，共享 64 条 ring 上限。
- 活跃 trace 占用调试句柄，期间内存/patch/maps/threads 等命令可能返回
  `OwnedByAnotherProcess`。
- PM hook 保持被等到的进程暂停；手工 wait 后必须 `start-process`。
- `trace-timeline` 只合并同一 boot 的 system tick。
- profile 标签是消费方提供的语义，不会替代协议字节验证。

## 数据产品

- `trace-dump`：读取当前保留 ring，生成版本化 JSONL。
- `trace-run --output`：等待结束并导出，同时嵌入 plan target ID/process 信息。
- `trace-report`：按 profile entry/return 计算返回值。
- `trace-ipc`：解码 HIPC/CMIF，配 transaction，汇总 domain object。
- `trace-timeline`：把同一 boot 的多个会话、transaction 和 domain event 放到一条时间轴。
- `trace-analyze` / `trace-diff`：统计和 A/B 比较。

临时 profile、trace、截图放消费仓库被忽略的 `build/scratch/`，不要提交调查噪声。
