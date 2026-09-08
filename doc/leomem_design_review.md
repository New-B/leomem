# LeoMem 系统机制设计要点与风险审视

> 本文基于 `LeoMem_Asplos27_Atm_1.pdf` 对 LeoMem 的系统设计进行结构化整理与批判性分析。
> 文中将“论文已经明确描述的机制”和“尚未闭合、需要进一步确认的内容”分开记录；后者不应被视为系统当前已经实现或保证的行为。

## 1. 一句话定位

LeoMem 是一个用户态、4KB 块粒度、基于 RDMA 的分布式共享内存系统。它试图通过三个数据平面控制点减少远程访问和一致性维护造成的流量放大：

1. 使用块级访问画像决定是否缓存远程块以及是否迁移写所有权；
2. 使用带版本号的惰性失效，在同步后按需验证缓存副本；
3. 按目的节点聚合细粒度远程写，并在合适的通信上捎带发送。

LeoMem 的核心价值主张不是单纯降低一次 RDMA 访问的延迟，而是减少无收益的块搬运、所有权震荡、失效广播、重复 refetch 和碎片化小写请求。

## 2. 系统模型与基本假设

### 2.1 内存组织

- 每个节点贡献一段连续本地内存，所有节点的内存共同组成全局逻辑地址空间。
- 全局地址可以解析为 home node 和 home node 内部偏移。
- 系统以 4KB block 为管理、缓存与一致性控制粒度。
- 应用通过 LeoMem API 访问共享地址；运行时负责地址翻译、缓存查找、目录管理、权限协调和 RDMA 通信。

### 2.2 每个块的三个角色

| 角色 | 论文中的职责 | 关键状态 |
| --- | --- | --- |
| Home node | 保存权威的已提交副本和目录元数据 | owner、sharer set、version、transition state |
| Owner node | 持有独占写权限和可能尚未提交的 dirty copy | 本地 dirty data、写画像、请求队列 |
| Sharing node | 持有只读缓存副本 | cached version、目录代际、替换分数 |

约束如下：

- 一个块最多只有一个 owner；
- 一个块可以同时存在多个只读 sharing replicas；
- 当 owner 有未提交写时，home copy 可能落后于 owner copy；
- 普通 reader 可以在当前同步区间内读取 home 上的旧 committed version；
- 跨越同步边界后，旧 replica 在复用前必须进行版本验证。

### 2.3 一致性模型

LeoMem 声称采用 release consistency：

- release 之前的写必须在相应 acquire 之后可见；
- barrier 被视为参与节点的 release 加 acquire；
- 同步区间内部允许缓存副本暂时落后；
- acquire/barrier 后，旧缓存不能未经验证直接复用。

这一模型为延迟提交、惰性验证和写聚合提供了空间，但也要求系统严格定义 release 的完成条件、版本推进点以及跨节点写入的可见性路径。

## 3. 总体架构

每个节点上的 LeoMem runtime 包含三个主要模块：

- Profiler：维护块级读写画像，给出缓存和 ownership 决策依据；
- Coherence controller：维护 home/owner/sharer 状态、版本与同步后验证；
- Per-destination batch buffers：缓存并聚合同一目的节点的细粒度远程写。

一次访问的大致分流如下：

```text
DSM access
  |
  +-- home-local block ----------------------> local access
  |
  +-- valid local cached block --------------> local access
  |
  +-- previous-generation read replica ------> validate version at home
  |                                               |-- equal: promote and reuse
  |                                               `-- stale: discard/refetch
  |
  +-- remote read miss ----------------------> profile + admission decision
  |                                               |-- reject: requested data only
  |                                               `-- admit: fetch/cache full 4KB
  |
  `-- remote write --------------------------> uncached update / redirect / ownership
```

## 4. 机制一：Profiling-Guided Admission

### 4.1 块级画像

LeoMem 分别维护 read-side 和 write-side profile。每个 4KB block 被划分为 64 个 64B region：

- RAT（Region Access Table）：64-bit bitmap，表示最近窗口中访问过哪些 64B region；
- `A_Cnt`：最近窗口中的访问次数；
- BSTW（Bucketed Sliding Time Window）：将时间窗口分为若干可循环复用的 bucket。

论文默认参数：

- 窗口长度 `T = 1000 ms`；
- bucket 数量 `K = 4`；
- profile 分别统计读和写。

设计采用事件驱动更新：仅在新访问到达时推进窗口、清除过期 bucket，不进行连续采样、周期性衰减或全表扫描。

请求节点把目标块的当前 profile 捎带给 home。home 不保存完整 profile，只保存 owner、sharer set 和 version 等权威状态。

### 4.2 Admission score

空间覆盖率与最终分数为：

```text
Coverage = popcount(RAT) / 64
Score    = A_Cnt * Coverage
```

默认 admission threshold 为：

```text
Theta_adm = 0.5
```

论文给出的直观例子：

- 只访问一个 64B region 一次，score 为 `1/64`，不会缓存；
- 重复访问同一个 64B region，需要 32 次才达到 0.5；
- 六次访问六个不同 region 时，score 为 `6 * 6 / 64 > 0.5`，会触发缓存。

### 4.3 Read admission

远程 read miss 时：

1. requester 更新并携带 read-side profile；
2. home 计算 admission score；
3. score 低于阈值时，只返回当前请求的数据，不安装完整缓存块；
4. score 达到阈值时，home 返回完整 4KB block 和 version；
5. requester 将其安装为 read replica。

目标是避免一次性或低复用访问污染缓存并扩大 sharer set。

### 4.4 Write admission

论文对读缓存和写缓存使用相同 admission rule，但使用独立的 write-side profile。

当块没有 owner 时：

- score 低：沿 uncached remote-update path 处理；
- score 高：请求者可以被授予 ownership，并获得 block state。

需要区分：admission rule 只决定“是否值得缓存/拥有”，并不单独决定数据从哪里取得或请求最终由哪个节点序列化。

### 4.5 合理性与收益

- 把 cache admission 从“miss 即缓存”改成“积累一定复用证据后缓存”，方向合理；
- 空间覆盖和访问次数结合，比单纯访问频率更能识别块级空间局部性；
- profile 在 requester 侧维护，避免 home 为所有 requester 保存完整历史；
- 事件驱动窗口降低无访问块的后台维护开销。

### 4.6 需要警惕的问题

#### 4.6.1 Score 没有直接对应成本收益

`A_Cnt * Coverage` 是启发式指标，不是缓存收益减去数据移动与一致性成本的模型。例如：

- 六次访问六个 region 只消费 384B，却足以触发 4KB fetch；
- 重复访问一个 64B region 要等到第 32 次才缓存，可能已经支付了过多远程访问；
- read caching、write ownership 和 full-block writeback 的成本结构不同，却共用同一个公式和阈值。

需要通过 break-even 分析或更完整的敏感性实验解释阈值为何合理。

#### 4.6.2 Profile 只反映单个 requester 的局部视角

当前 score 没有直接考虑：

- sharer 数量；
- 其他写者的竞争强度；
- home/owner 的负载；
- 当前网络拥塞；
- 更新大小和同步频率；
- ownership transfer 与随后 writeback 的成本。

因此，高 score 不一定代表迁移或缓存具有全局收益。

#### 4.6.3 Event-driven window 的过期语义需要明确

当 owner 长时间不产生本地写，但收到 contender 的迁移请求时，应先把 owner profile 推进到当前时间并清理过期 bucket，再比较两个 score。否则 owner 的历史高分可能长期阻止正确迁移。论文目前没有明确这一比较细节。

#### 4.6.4 Profile 存储成本未量化

每个被画像的块需要分别维护读写 bucket、bitmap、counter 和 timestamp。论文只评估了 fast-path CPU overhead，没有报告：

- 每块 profile 的实际字节数；
- profile table 的上限与回收策略；
- 数百万活跃块下的总内存占用；
- 多线程访问同一 profile 时的同步成本。

## 5. 机制二：Ownership Migration Control

### 5.1 迁移规则

当一个远程 write 已经通过 admission，但块当前由其他节点拥有时，LeoMem 使用 hysteresis：

```text
incoming_score >= 1.2 * owner_score
```

即新写者的分数至少高出当前 owner 20% 才迁移 ownership。

home 只记录 owner pointer，不持续保存 owner profile。出现候选写者时，home 把候选 score 随请求转发给当前 owner，由 owner 使用本地 write-side profile 完成比较。

### 5.2 普通写与重定向

当已有 owner 时：

- home 不直接把写应用到 committed home copy，因为该副本可能比 owner dirty copy 更旧；
- 未触发迁移的写被重定向到 owner；
- owner 维护 per-block request queue，并按序应用接受的写；
- 写只有在稳定 owner 上应用后才可确认。

### 5.3 论文描述的 handoff 流程

触发迁移后，旧 owner：

1. drain 已经接受的写；
2. pin 目标块并标记为 transferring；
3. 停止接受新的普通写；
4. 把迁移期间的新写收集并重定向到新 owner；
5. 对最新 dirty block 增加 version；
6. 将同一 versioned block 提交给 home，并直接发送给新 owner；
7. home 应用 committed block 并更新 owner pointer；
8. 新 owner 成为后续写的序列化点。

### 5.4 合理性与收益

- hysteresis 能过滤短时突发写和 score 小幅波动；
- ownership 保留在稳定主写者附近，可避免每次写都拉取整个块；
- home 不直接修改可能陈旧的副本，避免覆盖 owner 上的较新 dirty state；
- handoff 前 drain accepted writes 是避免丢写的必要步骤。

### 5.5 需要警惕的问题

#### 5.5.1 三方迁移协议没有完整状态机

当前描述尚未明确：

- home commit、new owner install、owner pointer update 的精确顺序；
- 哪个事件是 ownership transfer 的线性化点；
- new owner 何时可以处理和确认新写；
- 迁移消息重复、乱序或到达旧 owner 时如何识别；
- 是否有 ownership epoch/token 防止过期 owner 继续服务；
- home、old owner、new owner 的 transition 状态如何原子收敛。

建议在论文中给出显式状态机和消息时序图，而不只用文字说明 pin、retry 和 redirect。

#### 5.5.2 20% margin 仍可能震荡

相对 margin 没有考虑迁移的固定成本，也没有 cooldown。例如两个写者的 score 交替超过对方 20% 时仍会来回迁移。可以考虑：

- 最小 ownership residence time；
- 显式 migration cost amortization；
- score 超过绝对阈值且相对领先；
- 迁移后的短期惩罚或 exponential backoff。

#### 5.5.3 requester 与 owner score 的可比性

两个 score 来自不同节点的本地窗口。虽然不必依赖严格同步时钟，但必须保证：

- 窗口长度和 bucket 推进规则完全一致；
- owner 比较前进行时间过期处理；
- requester 的 remote-write profile 与 owner 的 local-write profile 统计口径一致；
- retry 或 redirect 不会重复计数。

## 6. 机制三：Versioned Lazy Invalidation

### 6.1 版本维护

- home 为每个块维护单调递增 version；
- read replica 保存 fetch 时对应的 version；
- owner 可以在同步区间内本地修改 dirty copy；
- owner release/barrier 时把 dirty data 提交到 home；
- home 先应用数据，再推进 version；
- commit 时不向 sharers 广播 invalidation。

### 6.2 两代共享缓存目录

每个节点维护两个 read-cache directory：

- current directory：本同步区间新 admission 或已经验证的副本；
- previous directory：上个同步区间有效、当前区间尚未验证的副本。

acquire/barrier 时：

1. 丢弃旧 previous directory；
2. current directory 变为新的 previous；
3. 创建空 current directory。

这一操作只改变目录代际，不扫描每个副本，也不立即发起版本检查。

### 6.3 Lookup 行为

- 命中 current：直接读取；
- 命中 previous：向 home 验证 cached version；
- version 相同：提升到 current 并读取；
- version 不同：本地失效，转向普通 remote path；
- refetch 后是否重新缓存，仍由更新后的 admission profile 决定。

### 6.4 合理性与收益

- 写者不承担与 sharer 数量成正比的 invalidation fanout；
- 同步点不扫描整个 cache；
- 没有被再次访问的副本不产生 validation 流量；
- 仍然有效的副本可以跨同步区间保留，避免 conservative SI 的无效 refetch；
- stale replica 在真正使用前检测，符合 release consistency 的基本思路。

### 6.5 需要警惕的问题

#### 6.5.1 VLI 仍然是一种固定策略

论文以“固定 WI/SI 不能适应动态 sharing”作为动机，但 LeoMem 对所有块统一使用 VLI，并没有在 WI、SI 和 VLI 之间动态选择。

当 sharer 很少、写很少、同步频繁且大量 clean replicas 每轮都会复用时，逐块 version validation 可能比只 invalidating 实际修改块更昂贵。因此论文更适合声称 VLI 提供了目标 workload 下更好的折中，而不是已经消除固定 coherence policy 的局限。

#### 6.5.2 Home 可能成为 validation hotspot

每个跨同步区间首次复用的块都要查询 home version。随着节点数、同步频率和 working set 增长，home 的 NIC、CPU 和目录可能成为热点。现有评估最多 16 个节点，尚不能证明更大规模下的扩展性。

#### 6.5.3 数据和 version 的原子性需要明确

必须保证：

- home 在新数据完整可见后才暴露新 version；
- validation 不能读到“新 version + 旧数据”或“旧 version + 部分新数据”；
- refetch 得到的数据和 version 属于同一个 committed snapshot；
- RDMA write、doorbell、completion 和 CPU metadata update 的 ordering 被明确规定。

#### 6.5.4 一般 acquire/release 下的目录代际语义不完整

两代目录在全局 barrier/epoch workload 中很自然，但一般程序可能存在：

- 一个节点上多个线程；
- 多把锁和不同 synchronization domain；
- concurrent acquire；
- cache lookup 与 directory rotation 并发。

论文需要说明 rotation 是 node-wide、thread-local 还是 synchronization-object-specific，以及如何防止某个线程错误复用另一个同步上下文中标记为 current 的副本。

#### 6.5.5 Version wraparound 与恢复语义未讨论

虽然实践中可以使用宽 version 避免短期回绕，但仍应说明 version 位宽、回绕处理、节点重启或元数据恢复后的行为。

## 7. 机制四：Destination-Aware Update Propagation

### 7.1 Per-destination buffers

每个源节点为不同目的节点维护独立 update buffer。每个 entry 包含：

- block identifier；
- offset；
- size；
- payload；
- local sequence number。

目标是在多个小写落到同一远端节点时合并消息，摊薄协议、RDMA transaction 和接收端排队开销。

### 7.2 Flush 条件

论文给出三类触发条件：

1. 累积 payload 达到默认 4KB；
2. release/barrier 完成之前；
3. 即将向相同目的节点发送 read 或 version-validation 请求时，将写 piggyback 到该通信中。

接收端按照同一 source-destination 流的 sequence order 应用更新。如果 response 依赖这些写，接收端先应用 piggybacked writes，再产生 response。

### 7.3 合理性与收益

- 小写合并能显著减少消息和 doorbell 数量；
- 按 destination 而不是 block 聚合，可跨多个 block 摊薄成本；
- piggyback 利用原本就会发生的通信；
- release 前 flush 为延迟传播提供了基本的同步边界。

### 7.4 需要警惕的问题

#### 7.4.1 Batching 与 ownership admission 的顺序风险

同一 requester 可能先产生若干低分 buffered writes，随后 profile 达到阈值并申请 ownership。若 ownership request 没有先 flush 并确认旧写，则较旧 batch 可能在较新的 owner-local write 之后到达并覆盖它。

需要规定：

- 发起 ownership admission/migration 前必须 flush 哪些 buffer；
- flush completion、ownership install 和后续本地写之间的 happens-before；
- 旧 batch 到达新 owner epoch 时如何处理。

#### 7.4.2 “目的节点”可能动态变化

当块没有 owner 时，目的节点通常是 home；存在 owner 时，写可能被重定向到 owner；迁移时目的节点又会变化。仅按地址解析得到 home 并分桶不足以说明最终路由。

应明确 buffer 是按 home、当前 owner 还是最终 transport peer 建立，以及 stale routing information 如何检测和重试。

#### 7.4.3 Per-source ordering 不等于完整同步语义

论文只承诺同一 source-destination 的 sequence order，但还需要说明：

- 一个 source node 上多个 application threads 如何编号；
- 多个 QP 或多个 batch 乱序到达时如何等待缺失 sequence；
- 多个 source 同时写同一块时由谁建立最终顺序；
- 跨 destination 写在 release 时如何共同完成；
- write acknowledgment 是“网络到达”“receiver apply”还是“home committed”。

#### 7.4.4 参数定义存在不一致

设计部分使用默认 4KB payload threshold；敏感性实验则使用默认最大 32 个 buffered updates，并再次使用符号 `K`。而 `K` 已经用于 BSTW bucket 数量。论文需要统一：

- 是按字节数、entry 数量还是二者任一条件 flush；
- 4KB 与 32 entries 的关系；
- 两类参数使用不同符号。

## 8. 机制五：Bounded Shared-Cache Management

### 8.1 代际回收

每次 acquire/barrier 时，旧 previous directory 中未被再次验证和提升的 clean replicas 被直接回收，无需 writeback。

这使未活跃副本最多跨越有限数量的同步区间，避免永久占用缓存空间。

### 8.2 容量压力下的替换

当回收旧 previous 后容量仍不足时，从 current directory 选择 victim：

- 首先选择 read-side locality score 较低的副本；
- score 相近时使用本地 LRU 打破平局；
- read replica 是 clean copy，因此 eviction 无需 writeback；
- 可以异步通知 home 从 sharer set 删除该节点；
- home 暂时保留 stale sharer entry 不影响正确性，因为本地已经不会再服务该副本。

### 8.3 需要警惕的问题

- cache capacity 的默认值和分配方式没有给出；
- 替换 score 的精确定义和更新时机不够明确；
- stale sharer metadata 的累积、清理和内存成本没有评估；
- LeoMem 不做 writer-side invalidation 时，维护完整 sharer set 的必要性需要解释；
- 多线程 lookup、promotion、rotation 和 eviction 之间的并发控制没有展开。

## 9. 最关键的正确性风险

### 9.1 非 owner 写入后的 release 可见性

这是当前设计描述中优先级最高的问题。

考虑如下执行：

```text
Block X owner = A
B writes X     -> request is redirected and applied to A's dirty copy
B releases
C acquires
C reads X      -> ordinary reads are served from home's committed copy
```

release consistency 要求 C 能看到 B 在 release 前的写。但论文只说明 owner 自己 release/barrier 时会把 dirty copy 提交到 home。如果 B 的 release 不强制 A commit，则 home 仍可能保存旧值。

论文必须在以下方向中明确选择一种或给出等价协议：

1. B release 时等待所有被远端 owner 接收的写完成，并要求对应 owner commit/version-bump 到 home；
2. home 记录 dirty owner，acquire 后的 read/validation 从 owner 获取最新 committed-for-release state；
3. 非 owner 写不进入 owner 私有 dirty state，而是使用另一套能够由 B release 完成提交的日志协议。

仅仅“写已在 owner 上应用”不足以证明它在相应 acquire 后通过 home 可见。

### 9.2 Uncached update 的 version 推进规则

当无 owner 时，低分写直接在 home 应用。论文尚未说明 version 如何变化：

- 每条写推进一次；
- 每个 batch 中每个受影响 block 推进一次；
- 只在 source release 时推进；
- 数据在 release 前可以提前可见，但 version 必须与其一致。

如果 home 数据已改变而 version 未改变，旧 replica 可能验证成功并读取旧值。该问题必须与 batching flush 和 release completion 一起定义。

### 9.3 Handoff 的线性化与 epoch

需要给 owner pointer 和所有消息增加明确 epoch/token，并定义：

- old epoch 的写在何处截止；
- transferred block 包含哪些 sequence；
- new epoch 从何处开始；
- home 何时允许 read-cache fill；
- stale request 如何拒绝或重定向；
- ACK 对应哪个稳定状态。

否则文字上的“drain + redirect + retry”仍可能隐藏丢写、重复写或双 owner 窗口。

### 9.4 Synchronization completion 条件

release/barrier 至少需要等待：

- 本节点所有相关 destination buffers 已发送；
- receiver 已按序应用更新；
- 必须进入 home committed state 的数据已经提交；
- 对应 block version 已推进并对 validation 可见；
- ownership transition 中相关写已经落入确定 epoch。

论文目前主要描述“flush before release returns”，但 flush 的完成语义还不够精确。

## 10. 设计主张与证据之间的差距

### 10.1 “Coordinated data plane”目前更像机制组合

三个机制共存于同一运行时，并在同步、admission 和 piggyback 上发生交互；但论文尚未展示一个真正统一的跨机制优化目标。例如：

- profile 没有同时估算 cache benefit、ownership cost 与 batching benefit；
- coherence policy 不根据 profile 或 sharer 状态变化；
- batching 没有明确参与 ownership 决策；
- cache pressure 没有反馈到 admission threshold。

因此“coordinated”应谨慎表述，或者增加跨机制决策与交互实验。

### 10.2 工作负载适用范围

LeoMem 最可能受益于：

- barrier/epoch 驱动的程序；
- 跨迭代仍有较稳定 read reuse；
- 每块只有一个长期 dominant writer，其他写者较弱；
- 细粒度更新存在明显 destination locality；
- 同步周期足够长，可以摊薄 validation 和 commit。

可能不利的情况包括：

- 多写者强度接近且快速切换；
- 极短同步周期；
- 每轮复用大量 clean blocks、实际更新很少；
- 随机目的节点的小写，难以形成 batch；
- 访问 phase 短于或显著长于固定 1 秒窗口；
- 只访问少量 64B region，但访问次数刚好触发 full-block ownership；
- home version validation 形成热点。

## 11. 评估证据中需要警惕的内容

### 11.1 微基准可能偏向目标机制

- Ownership benchmark 使用 70/30 dominant/noisy writer，天然适合 hysteresis；
- VLI benchmark 每轮只更新 32/256 blocks，同时存在较多 readers，天然放大 WI fanout 和 SI refetch；
- Batching benchmark 让 80% 更新集中到两个目的节点，具有很强 destination locality。

这些实验适合证明机制在预期场景中有效，但还不足以说明在边界和反例场景中稳健。

### 11.2 建议补充的反例实验

- 50/50、55/45、70/30、90/10 多写者分布；
- dominant writer 随时间切换的 phase-change workload；
- 不同 phase 长度相对于 1 秒 profiling window 的变化；
- destination locality 从完全随机到高度集中；
- synchronization frequency sweep；
- sharer count、修改比例和复用比例的二维扫描；
- cache capacity 与工作集大小的变化；
- home hotspot 和倾斜 placement；
- block size、region size、migration margin 和 bucket 数量敏感性；
- batching 与 ownership 同时开启时的交互实验。

### 11.3 缺少的系统成本

建议报告：

- profile metadata bytes/block；
- directory metadata bytes/block；
- sharer set 维护和 stale entry 数量；
- current/previous directory rotation 成本；
- validation RPC 数量和 home CPU/NIC utilization；
- ownership handoff 的消息数、字节数和停顿时间；
- buffer memory、flush rate、batch size distribution；
- P50/P95/P99 延迟，而不仅是平均性能；
- 应用级重复次数、误差线和统计稳定性。

### 11.4 Baseline 公平性

论文声称所有系统使用相同硬件、块大小、线程数、数据布局和应用实现，但仍应进一步给出：

- 各 baseline 的版本、commit 和关键配置；
- 是否使用相同 consistency semantics；
- 是否为 baseline 增加或移除了原生 runtime 功能；
- Argo、GAM、Itoyori 的 cache size 和同步策略；
- 应用结果一致性验证；
- 是否包含 task-runtime、GC、初始化、数据加载等时间。

## 12. 建议优先补全的论文内容

### P0：正确性闭环

1. 定义所有 write path 的状态转移：home update、owner redirect、ownership admission、handoff；
2. 定义每条路径的 version-bump 时机；
3. 定义 release/barrier 的完成条件；
4. 解决非 owner 写入后由 requester release 的可见性问题；
5. 给出 ownership epoch 和 handoff 线性化点；
6. 解释一般多线程 acquire/release 下的 cache generation 语义。

### P1：实现可信度

1. 给出消息协议和关键 RDMA ordering；
2. 给出 metadata layout 与空间开销；
3. 给出并发数据结构和锁/原子操作；
4. 说明 batch 重定向、重试、去重和 sequence gap 处理；
5. 说明 version 位宽与 wraparound。

### P2：论证完整性

1. 补充 adversarial microbenchmarks；
2. 扩展参数敏感性与 cache-capacity 实验；
3. 报告机制交互而不仅是单项 ablation；
4. 量化 home bottleneck；
5. 收紧“fixed policy”和“coordinated”相关表述；
6. 更清楚地区分新机制、已有技术和集成贡献。

## 13. 可用于检查实现的核心不变量

后续审查代码时，可以围绕以下不变量进行验证：

1. **Single-owner invariant**：任意 block epoch 中最多一个节点能够确认 owner-local writes。
2. **Committed-read invariant**：home 返回的数据与返回的 version 属于同一个完整 committed snapshot。
3. **Release visibility invariant**：某线程 release 返回前的所有写，在对应 acquire 后必定能通过 home、owner 或验证路径观察到。
4. **No-stale-reuse invariant**：跨 acquire/barrier 后，旧代 replica 未验证前不能向应用返回数据。
5. **Migration completeness invariant**：handoff block 包含旧 epoch 截止点之前的全部已确认写。
6. **No-late-overwrite invariant**：旧 epoch 的 buffered update 不能在新 epoch 的写之后被应用。
7. **Per-source-order invariant**：同一 source 的程序顺序写在 receiver 上不会颠倒。
8. **Release-drain invariant**：release 覆盖的每个 destination stream 都完成到足以满足可见性的阶段。
9. **Version-progress invariant**：任何可能使已缓存副本过期的 committed update 都必须导致可观察的 version 变化。
10. **Validation freshness invariant**：与某次 acquire 对应的 validation 不能先于该 acquire 所同步的 release commit。

## 14. 总体判断

LeoMem 的性能问题识别是有价值的：缓存本身可能放大 ownership、coherence 和小写通信成本，DSM 不应默认把所有 miss 都转换成长期缓存状态。Profiling-guided admission、VLI 和 destination batching 各自也具有清楚的工程直觉。

但当前版本更像一套面向 barrier-driven analytics workload 的高性能优化框架，而不是已经完全定义和证明的一般性 DSM coherence protocol。最需要优先处理的不是继续增加性能结果，而是补全以下四个协议闭环：

1. 非 owner 写入与 requester release 的关系；
2. uncached/batched update 的 version 规则；
3. batching 到 ownership transition 的顺序；
4. 三方 ownership handoff 的状态机与线性化点。

这些问题一旦明确，论文中的性能机制、实现细节和实验设计才能围绕同一套可验证语义展开。
