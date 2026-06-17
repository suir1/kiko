# kiko 产品与技术计划

本文档汇总连通性、UDP 辅助穿透、relay 对齐、AI 辅助传输的设计结论与实施优先级。

**产品目标**：像 croc 一样，连 **公共或自建 relay** 即可传文件；LAN 发现是加速项，不是前提。  
**连通原则**：打洞信息与时机 **依赖 relay**；直连是 relay 牵线后的优化，不能替代 relay 兜底。

---

## 1. 产品目标与现状

| 目标 | 说明 |
|------|------|
| 公网 / 自建 relay | 双方同一 relay（或默认公共 relay），无需端口转发 |
| E2E 安全 | relay 盲转发；PAKE + XChaCha20-Poly1305 + zstd |
| LAN 可选加速 | 组播 + embedded relay；失败回退公网 relay |
| 与 croc | 协议不互通；体验对齐，不字节级复刻 |

### 1.1 相对 croc 的缺口

| 项 | croc | kiko 现状 |
|----|------|-----------|
| 默认 relay | 公网 `croc.schollz.com:9009` | 公网 `106.53.170.243:9000` |
| relay 密码 | `--pass` / `CROC_PASS` | ✅ `--relay-pass` / `KIKO_RELAY_PASS` |
| 配置记忆 | `--remember` | ❌ |
| room TTL / keepalive | ✅ | ✅ TTL + waiting keepalive |
| pre-pipe TCP punch | ❌ | ✅ `try_direct`（需变聪明，少拖 relay） |
| UDP STUN / 打洞 | ❌ | ✅ STUN 探测（U-A，`--udp-probe`） |
| UDP 传数据 | ❌ | ❌（远期单独立项） |

### 1.2 kiko 已有（保持）

ping/pong、send/recv 并行 rendezvous、embedded relay、IPv6-first dial、`AdaptivePuncher`、pipe 上 `ips?`、mux、`--local` / `--no-direct` 等。

---

## 2. 网络现实：谁在用哪种拓扑

**默认假设**：陌生人公网互传时，**双端 NAT 非常普遍**（家庭路由、手机 CGNAT、公司 Wi‑Fi）。  
**「双端 NAT 能 P2P 打洞成功」** 是少数——取决于 Cone/Symmetric 组合；**越严越靠 relay**。

| 拓扑 | 常见度 | 可靠路径 | P2P 直连 |
|------|--------|----------|----------|
| 双端 Cone NAT | 较常见 | relay | UDP/TCP 有时 |
| Cone + Symmetric | 常见 | relay | 难 |
| 双 Symmetric / 双 CGNAT | 多（尤其移动） | relay | ≈0 |
| 同 LAN 可互访 | — | embedded / 组播 | 高 |
| AP 隔离 | 热点常见 | **仅公网 relay** | ❌ |
| 一端公网 / 端口转发 | 少 | TCP direct 或 relay | 高 |

**设计推论**：

1. **relay pipe 必须 100% 可靠**（主路径）。
2. **直连是 bonus**；失败时 **不能串行阻塞 relay**（驱动 U-C）。
3. **STUN 先测量再策略**（驱动 U-A），避免 kiko 现有 `try_direct` 在 symmetric 上白等。

### 2.1 croc 做了什么、没做什么

| | croc |
|---|------|
| ✅ | TCP relay pipe、LAN 组播、embedded relay、pipe 上 `ips?` |
| ✅ | UDP 仅用于组播发现 + `Dial udp 8.8.8.8:80` 取本机 IP |
| ❌ | STUN、UDP 打洞、TCP WAN punch、TCP+UDP 竞速、QUIC/KCP |

kiko 的 U-A/U-C 是 **补 kiko 自身 try_direct 的债**，不是 croc 对齐必选项。

---

## 3. 连通架构

### 3.1 现状数据流

```text
ping/pong → hello → peer（交换地址）
    → try_direct（TCP：receiver connect + sender accept）  ← 与 relay 部分串行，待改
    → 失败：relay_ready → relay pipe → ips? → PAKE → TCP 传输
```

### 3.2 目标分层（防臃肿）

```text
ConnectivityOrchestrator          ← 选路、race、deadline
  ├── RelayRendezvous             （已有：ping/hello/peer/pipe）
  ├── UdpProbe / StunClient       （U-A：仅探测）
  ├── UdpPunchCoordinator         （U-B：可选，A 判定 punchable 才启用）
  ├── TcpDirectStrategy           （已有 try_direct + AdaptivePuncher）
  └── RuleScheduler + profile     （规则默认；AI 可选合并）

TransferSession                   ← 不变：PAKE + zstd + 帧，只认 TcpSocket
```

**原则**：UDP **只参与连通**；文件字节 **始终 TCP**（U-A/U-B/C 阶段不引入 UDP 数据面）。

---

## 4. UDP 辅助连通：方案 U-A / U-B / U-C

> 下文 **U-A/B/C** 指 UDP 连通方案，与 AI 模块编号无关。

### 4.1 方案定义

| 方案 | 做什么 | 数据面 | 工程量 |
|------|--------|--------|--------|
| **U-A. UDP 只做探测** | STUN 测 mapped 地址 / NAT 类型 → 优化 TCP 候选与 `skip_direct`、超时 | TCP | **小** |
| **U-B. UDP 打洞 + TCP 跟进** | UDP simultaneous poke 对齐 mapping → 同 reflexive **TCP connect** | TCP | **中** |
| **U-C. 路径竞速** | TCP direct、（可选 U-B）、relay pipe、LAN relay **并行 race** | TCP | **中**（编排为主） |

### 4.2 按场景收益

| 场景 | U-A | U-B | U-C |
|------|-----|-----|-----|
| 同 LAN 可互访 | 低 | 低 | 低 |
| AP 隔离 | 中（早 skip） | 低 | **中–高**（早进 relay） |
| 一端公网 | 低–中 | 低 | 中 |
| 双 Cone NAT | 中–高 | **高** | **高** |
| Cone + Symmetric | **高** | 中 | 中–高 |
| 双 Symmetric / CGNAT | **高**（早放弃） | 低 | 中 |
| VPN + 热点 LAN | 中–高 | 低 | 中 |
| 陌生人 WAN（全体） | **中** | 中* | **中–高** |

\* U-B 的「中」= **少数用户高收益**，不是多数。

### 4.3 推荐优先级（ROI）

```text
P0  U-A   STUN + NAT 类 → RoutePlan（必做）
P0  U-C-lite  try_direct 与 relay 并行，不等 punch 超时（必做）
P1  U-C-full  统一 RouteExecutor（LAN / embedded / external / direct）
P2  U-B   仅当 U-A 判定 punchable（cone / noisy_linear）时启用
—   UDP 传数据（QUIC/KCP）  单独立项，不在本计划 MVP
```

**不要**：U-B 先于 U-A；三方案 v1 全上；指望 U-B 替代 relay。

### 4.4 U-A 技术要点

- 向 1–2 个 STUN 服务器发 Binding Request（可自建，feature flag `--udp-probe`）。
- 输出：`mapped_endpoint`、`nat_class`（open / cone / symmetric / unknown）、`delta_stats`（若多样本）。
- 写入 `RoutePlan`：
  - `symmetric` / `random` → `skip_direct: true`
  - `cone` → 更准确 `peer_candidates`、可调 `AdaptivePuncher` 超时
- **过滤 VPN tun/wg 接口**出 `local_candidates`（与 U-A 同批）。

### 4.5 U-B 技术要点（P2，条件启用）

- 经 relay 交换 **UDP punch 时序 token**（毫秒时间戳，非微秒「欺骗防火墙」）。
- 双方向对方 STUN-mapped 地址 **并发 UDP poke**（有界窗口 W ≤ 32）。
- poke 窗口内 **TCP connect** 同一 host:port。
- **不**做 LLM 单点 port 预言；不循环调 API。

### 4.6 U-C 技术要点

- **现状问题**：`try_direct` 与 relay 部分 **串行**，失败白等 1–3s 再 pipe。
- **目标**：`relay` 连接与 registration **始终并行**；direct 在独立 deadline 内尝试，**不阻塞** `peer` 已就绪后的 pipe。
- 与已有 send/recv **多 relay race** 合并为统一 `RouteExecutor::race_until_peer()`。

### 4.7 UDP 传数据（不在本计划）

| 潜在好处 | 何时值得 |
|----------|----------|
| 高丢包弱网、HOL 阻塞 | 跨国移动、丢包 >1–2% |
| QUIC 连接迁移 | 传大文件换网不断 |

对 croc/kiko 类「relay 兜底 + 多数链路尚可」：**TCP 传数据足够**。QUIC/KCP 记为 **Future/Transport-Research**，不与 U-A/B/C 捆绑。

---

## 5. AI 辅助（可选 BYOK）

### 5.1 原则

1. **测量在本地**；规则 + profile 覆盖 80%；AI 补模糊区与人话。
2. 打前 / 失败后 **各 ≤1 次** LLM（~300ms，超时 → `RuleFallback`）。
3. **不上传**：配对码 secret、文件路径/内容、PAKE 材料。
4. **不承诺**：对称 NAT 单口预言、微秒防火墙对齐、WAN 打洞「数倍」。

### 5.2 模块

| 模块 | 作用 | 优先级 |
|------|------|--------|
| **诊断官** | `doctor` + 错误码 → 人话（AP 隔离、VPN、双 NAT） | P1 |
| **路由顾问** | 读 `ConnectivitySnapshot` → 合并 `RoutePlan` | P2 |
| **端口摘要** | 读 `delta_mean/std`，建议 scan_window / abort | P3（U-A 之后） |
| **压缩/连接数** | 熵/heuristic 即可，**不必 LLM** | 规则 |

### 5.3 ConnectivitySnapshot（摘要）

```json
{
  "nat_self": "behind-nat",
  "stun_nat_class": "symmetric",
  "lan_discovered_count": 0,
  "vpn_detected": true,
  "relays": [{"kind": "external", "rtt_ms": 120, "pong_ok": true}],
  "punch": {"attempted": true, "failures": {"timeout": 3}, "direct_ok": false},
  "transfer": {"total_bytes": 500000000}
}
```

### 5.4 RoutePlan（AI 与规则共用，白名单校验）

```json
{
  "relay_order": ["lan:embedded", "wan:external"],
  "skip_direct": true,
  "udp_punch_enabled": false,
  "direct_timeout_ms": 0,
  "connections": 8,
  "reason": "symmetric_nat"
}
```

### 5.5 配置（计划）

```toml
# ~/.config/kiko/config.toml
[relay]
default = "relay.example.com:9000"   # 或 KIKO_RELAY  env

[ai]
enabled = false
base_url = "https://api.openai.com/v1"
api_key_env = "OPENAI_API_KEY"
model = "gpt-4o-mini"
timeout_ms = 8000
```

```bash
kiko doctor --json [--ai-explain]
kiko send ... --ai-route-plan-only
kiko send ... --ai-route
kiko send ... --udp-probe          # U-A feature flag
```

---

## 6. relay 与 croc 对齐

| 优先级 | 项 |
|--------|-----|
| P0 | `KIKO_RELAY` / 默认公共 relay URL |
| P0 | 自建 relay 文档、防火墙端口说明 |
| P1 | `--relay-pass`、room TTL、room full、等 peer keepalive |
| P1 | 多端口 banner（mux 端口列表） |
| P2 | ips? 加密、ExternalIP UX |
| — | 兼容 croc relay 二进制 | 不做 |

---

## 7. 典型场景 playbook

### 7.1 公网 / 自建 relay（主场景）

```bash
kiko send ./f --relay relay.example.com:9000
kiko recv <code> --relay relay.example.com:9000 --out .
```

纯 pipe：`--no-lan --no-local --no-direct`（可选）。

### 7.2 同 LAN / 热点

默认自动（组播 + embedded + 公网并行）。**不必加参数**（与 croc 一致）。  
纯 LAN / A 开 VPN：`--local`。

### 7.3 AP 隔离

LAN 失败；默认仍会 try TCP reflexive。建议 `--no-direct` 或等 U-A 自动 skip。  
**靠公网 relay**。

### 7.4 双端 NAT WAN

**预期走 relay**；U-A/U-C 缩短失败等待，U-B 仅少数 Cone 组合可能直连。

---

## 8. 实施阶段

### Phase 0 — 基线与编排（P0）

- [x] `ConnectivitySnapshot`、`RoutePlan`、`RuleScheduler`
- [x] 重构 `transfer.cpp` 挂钩点（`RoutePlan` + profile）
- [x] `kiko doctor [--json]`
- [x] `~/.config/kiko/profile.json`（gateway 指纹 → 上次成功 plan）
- [x] `KIKO_RELAY` 环境变量 + README
- [x] 过滤 VPN 网卡出 `local_candidates`

### Phase 1 — U-A + U-C-lite（P0，连通 ROI 最高）

- [x] `src/connectivity.cpp`：STUN Binding + NAT 粗分类
- [x] STUN 结果并入 `AdaptivePuncher` / `skip_direct`
- [x] STUN 与 rendezvous **并行**（`--udp-probe` 开启时）
- [x] CLI：`--udp-probe`（默认 off）、`--no-direct` 与 plan 联动
- [x] 单元测试：plan 生成（`tests/connectivity_test.cpp`）

### Phase 2 — U-C-full + 诊断（P1）

- [x] 统一 `race_until_peer()`：LAN discovered / embedded / external / direct 同 deadline
- [x] 错误模板：AP 隔离、relay 不通、VPN、双 NAT（`kiko doctor`）
- [x] TUI/CLI 打印 `puncher.report()` + 建议
- [x] `--ai-explain`（BYOK，opt-in）

### Phase 3 — relay 产品化（P1）

- [x] 编译期默认 relay URL（`KIKO_DEFAULT_RELAY` CMake 选项）
- [x] `--relay-pass` / `KIKO_RELAY_PASS`、`kiko relay --pass`、room TTL / cleanup
- [x] room full、等待 keepalive ping
- [x] 单元测试 `tests/relay_test.cpp`
- [ ] 托管公共 relay + CI 公网冒烟（需部署 relay 后设 `KIKO_RELAY_SMOKE`）

### Phase 4 — U-B 条件打洞（P2，可选）

- [x] relay 交换 `punch_token`（时间戳）
- [ ] UDP poke + 有界窗口 + TCP follow（现有 `udp_punch` 仅作实验占位，默认不启用）
- [ ] 仅 `RoutePlan.udp_punch_enabled == true` 时启用
- [ ] 集成测试（需可控 NAT 环境或 staging）

### Phase 5 — AI 路由顾问（P2–P3）

- [x] `ai_client.cpp` / `ai_advisor.cpp`（OpenAI-compatible BYOK）
- [x] `--ai-route` / `--ai-route-plan-only`
- [x] 与 STUN/doctor 并行，400ms hard deadline

### Phase 6 — 传输启发式（P3）

- [x] 高熵 / 已压缩格式跳过 zstd（`compress: none` 帧）
- [x] 按 RTT + 文件大小调 `--connections`（`--auto-connections`）
- [ ] （远期）QUIC/KCP 调研 → 独立 `docs/transport-research.md`

---

## 9. 明确不做

| 项 | 结论 |
|----|------|
| LLM 猜单个 magic port | ❌ |
| 微秒级「欺骗防火墙」 | ❌ |
| WAN 打洞成功率数倍 | ❌ |
| U-B 替代 relay | ❌ |
| UDP 传数据作为 U-A/B/C 一部分 | ❌ 另立项 |
| 与 croc 协议互操作 | 非默认 |
| AI 热路径每 connect 调 API | ❌ |

**对外表述**：

> 规则与 STUN 记住怎么连；relay 永远兜底；AI 在说不清时帮你选路并用的人话解释。

---

## 10. 成功指标

| 指标 | 目标 |
|------|------|
| 自建 / 公网 relay 传文件 | 仅 `--relay` 或默认成功 |
| 双 NAT WAN | 100% relay pipe 完成 |
| 双 NAT 失败到 pipe | U-C 后 p95 额外等待 **< 500ms**（现 1–3s punch） |
| 同 LAN | 无参成功率 ≥ croc |
| U-A symmetric 判定 | 不再发起 >200ms 无效 TCP punch |
| doctor | 90% 常见失败有可操作建议 |
| AI | opt-in；单次传输 ≤2 次调用；失败不阻塞 |

---

## 11. 关键文件（计划）

| 模块 | 文件 |
|------|------|
| STUN / NAT | `src/stun_client.cpp`, `src/nat_probe.hpp`（新） |
| 编排 | `src/connectivity_orchestrator.cpp`, `src/route_scheduler.cpp`（新） |
| Snapshot / doctor | `src/doctor.cpp`（新） |
| Profile | `src/profile.cpp`（新） |
| UDP punch | `src/udp_punch.cpp`（新，Phase 4） |
| AI BYOK | `src/ai_client.cpp`, `src/ai_advisor.cpp`（新） |
| 连通 hook | `src/transfer.cpp`, `src/adaptive.cpp` |
| Relay 运维 | `src/relay_server.cpp` |
| CLI | `src/main.cpp` |
| TUI | `src/tui.cpp` |

---

## 12. 决策记录（摘要）

| 日期 | 决策 |
|------|------|
| — | TCP 传输 + relay 兜底；UDP 仅连通，不传文件（MVP） |
| — | U-A + U-C-lite 优先于 U-B |
| — | kiko `try_direct` 保留，由 STUN/plan 变聪明，非删除 |
| — | AI 主打诊断与 RoutePlan，不做 port oracle |
| — | 双端 NAT 为默认假设；P2P 为 bonus |

---

*最后更新：整合 UDP U-A/B/C、双 NAT 现实、croc 对比、AI 与分阶段优先级。*
