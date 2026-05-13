# Derive 函数状态映射审查

审查文件: `claude-desktop-buddy-s3/src/main.cpp` → `PersonaState derive(const TamaState& s)`
审查时间: 2026-05-04

---

## 整体架构

设备侧有两层状态系统：

```
derive(tama) → baseState     ← 来自 Codex/Claude 数据的"基础情绪"
activeState                            ← 当前实际显示的状态
oneShotUntil                           ← 一次性动画的截止时间
```

每帧循环：
1. `baseState = derive(tama)` — 根据 session 数据计算基础状态
2. 若有 one-shot 动画（`millis() < oneShotUntil`），`activeState` 覆盖
3. 否则 `activeState = baseState`

7 种状态来源：
- **derive()**：`P_SLEEP`, `P_IDLE`, `P_BUSY`, `P_ATTENTION`, `P_CELEBRATE`
- **oneShot（摇一摇）**：`P_DIZZY`（2秒）
- **oneShot（快速审批）**：`P_HEART`（2秒）
- **时钟逻辑**（仅充电+RTC有效+idle）：覆盖 `activeState`，含时间相关情绪
- **面朝下 nap**：跳过 sprite render，宠物睡觉

---

## 问题 1：`!connected` 返回 `P_IDLE` 而不是 `P_SLEEP`（已修复）

**代码**：
```cpp
if (!s.connected) return P_IDLE;
```

**`TamaState::connected` 定义**（`data.h`）：
```cpp
inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}
```
最后收到 JSON 帧超过 30 秒，`connected` 变为 `false`。

**影响**：
- Codex 没开 / daemon 未启动 / Claude 独占 BLE → buddy 超 30 秒收不到帧 → `connected = false` → derive 返回 `P_IDLE`
- buddy 显示"眨眼、四处看"（"我在等你工作"）
- PRD 定义中 `P_SLEEP`（眼睛闭着，缓慢呼吸）才是"断连"时的状态

**视觉不一致**：
- `P_SLEEP` = 断连/休息
- `P_IDLE` = 连接中但在等待

**实际缓解**：
- bridge heartbeat 帧含 `"tokens": 0` 和 `"msg": "Codex idle"` 时，`_lastLiveMs` 每帧刷新 → `connected` 仍为 true
- 只有 daemon 完全没连上 buddy 才触发 `!connected`

**修复**：`!connected` → `P_SLEEP`

---

## 问题 2：`P_BUSY` 阈值 `sessionsRunning >= 3` 过高（已修复）

**代码**：
```cpp
if (s.sessionsRunning >= 3) return P_BUSY;
```

**`sessionsRunning` 来源**：
- Codex 桥接的 snapshot 里 `running` 固定为 0（`protocol.py`）
- Claude Desktop 原生推送才有 `running > 0`

**影响**：
- Codex 场景下 `sessionsRunning` 永远是 0 → `sessionsRunning >= 3` 永远不成立 → **死代码**
- 即使 Claude Desktop 同时跑，也需要 3 个并发 session 才触发 busy
- buddy 永远不会因为 Codex session 而进入 `P_BUSY`

**修复**：改为 `sessionsRunning >= 1`，让任何运行中的 session 都能触发 busy

---

## 问题 3：`recentlyCompleted` 优先级高于 `sessionsRunning`

**代码**：
```cpp
if (s.recentlyCompleted)     return P_CELEBRATE;   // line 3
if (s.sessionsRunning >= 3)  return P_BUSY;         // line 4
```

**`recentlyCompleted` 来源**（`data.h`）：
```cpp
out->recentlyCompleted = doc["completed"] | false;
```
bridge 发 `completed: true` 时设置，新帧无 `completed` 字段时被 `| false` 覆盖。

**影响**：
- 如果 `recentlyCompleted` 为 true 但 `sessionsRunning >= 3`，先匹配到 `P_CELEBRATE`
- 但 `recentlyCompleted` 通常是一次的（新帧覆盖），正常情况下不会 sticky
- celebrate 打断 busy 的设计意图是正确的

**优先级设计**：celebrate 应该打断 busy → 优先级 OK

---

## 问题 4：`sessionsWaiting > 0` 但 `sessionsRunning == 0` 的清零时机不一致

**代码**：
```cpp
if (s.sessionsWaiting > 0) return P_ATTENTION;
```

**`sessionsWaiting` 来源**（`protocol.py`）：
```python
def build_prompt_snapshot(approval: ApprovalRequest) -> str:
    return _encode_line({
        "total": 1,
        "running": 0,
        "waiting": 1,    // ← 审批等待中
    })
```

**影响**：
- 用户按 A 后，设备端本地立即清除 `tama.promptId`（`main.cpp:1154`）
- 但 `tama.sessionsWaiting` 没清 → derive 仍返回 `P_ATTENTION`
- `drawApproval()` 检查 `tama.promptId[0] && !responseSent` → promptId 已清空 → HUD 不显示
- **视觉上**：buddy 显示 attention 状态（alert，LED 闪烁），但审批 HUD 已退了 — 状态和 UI 不一致

---

## 问题 5：`tokensToday` 没有被 derive 使用

**`TamaState` 里有**：
```cpp
uint32_t tokensToday;
```

**derive 没用它**。

**`tokensToday` 有值的情况**：
- Claude Desktop 原生推送包含 `tokens_today`
- Codex 桥接的 snapshot 里没有 `tokens_today`

**影响**：对 Codex 场景 `tokensToday` 永远是 0，无法区分"刚启动"和"跑了很久"。

---

## 完整状态流总结

```
derive() 输出       Codex场景       Claude场景         问题
────────────────    ──────────      ──────────         ────
P_SLEEP             ✅ 修复后       ✅ 深夜/nap          原: derive 不返回
P_IDLE              ✅ 修复后       ✅ 空闲              原: 断连/空闲混用
P_BUSY              ✅ 修复后       ✅ running≥1          原: Codex场景死代码
P_ATTENTION         ✅ waiting>0     ✅ waiting>0         未修复: 清零时机不一致
P_CELEBRATE         ✅ completed     ✅ completed         OK
P_DIZZY             ✅ shake         ✅ shake             oneShot, OK
P_HEART             ✅ 快速审批      ✅ 快速审批           oneShot, OK
```

**Codex 桥接场景下 derive() 实际有效分支**（修复后）：
1. `!connected` → `P_SLEEP` ✅（原 P_IDLE）
2. `sessionsWaiting > 0` → `P_ATTENTION` ✅
3. `recentlyCompleted` → `P_CELEBRATE` ✅
4. `sessionsRunning >= 1` → `P_BUSY` ✅（原 >= 3）
5. 兜底 → `P_IDLE` ✅

**未修复**：
- 问题 4：`sessionsWaiting` 清零时机不一致 — 设备端按 A/B 时应同时清空 `sessionsWaiting`

---

## 修复内容

### 修复 1：`!connected` → `P_SLEEP`
文件：`claude-desktop-buddy-s3/src/main.cpp:533`
改动：`return P_IDLE` → `return P_SLEEP`

### 修复 2：`sessionsRunning >= 3` → `sessionsRunning >= 1`
文件：`claude-desktop-buddy-s3/src/main.cpp:536`
改动：`>= 3` → `>= 1`

### 修复 3（连带）：设备端按 A/B 审批时清空 `sessionsWaiting`
文件：`claude-desktop-buddy-s3/src/main.cpp:1154-1156`（BtnA）和 `main.cpp:1191-1193`（BtnB）
改动：添加 `tama.sessionsWaiting = 0;`
