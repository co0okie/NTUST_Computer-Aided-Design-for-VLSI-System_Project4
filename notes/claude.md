# 研究日誌（claude 路線）

正式數據：`notes/claude-results.tsv`。合併到 `shared/best.tsv`：`python3 notes/update_best.py RESULTS.tsv`。

## 階段 1（2026-11-11）

### 1. 動機實驗：測資幾乎都是「資源主導」

各電路的 operation 數與深度（`shared/tmp/claude/stats.py`）：深度只有 3–37（C6288 為 179），
但單一 kind 的 `ceil(N_k/c_k)` 動輒數百到上萬。latency 幾乎完全由**瓶頸 kind 的機器有沒有閒置**決定。
baseline 的 lower bound（critical path 與 `ceil(N_k/c_k)`）與 height-priority list scheduling 都看不到這一點。

### 2. Lower bound：遞迴 resource tail + Jackson（`src/bounds.cpp`）

- `tail[i]`：op i 之後至少還要幾步。對 i 的後代集合 D，對每個 kind k、每個門檻 q：
  D 中 kind k、tail ≥ q 的 N 個 op 都要排在 i 之後，每步最多 c_k 個，最後一個之後還有 q 步，
  所以 `tail[i] ≥ ceil(N/c_k) + q`。N=1 時就是一般的 height。以反拓撲順序一次算完（後代的 tail 已是最終值）。
- `head[i]`：在 mirror 問題（所有邊反向）上算同樣的 tail，再加 1。
- 全域 bound：每個 kind 單獨看成「unit job、release = head、delivery = tail、c_k 台機器」，
  largest-tail-first（Jackson）對 unit job 是最佳的，取 `max(t + tail)`。忽略同 kind 之間的 precedence，所以是合法的放鬆。
- 需要 transitive closure（bitset，n²/8 bytes；big3 約 26 MB，超過 400 MB 就退回一般 height）。
- 結果：baseline heuristic 不變時，LB==UB 的 case 就從 27 大幅增加；codex 同時做了較簡單的
  closure-count 版本（59/76），可當 ablation 對照。

### 3. Heuristic（`src/heuristic.cpp`）

1. **tail-priority list scheduling**：priority 改成 resource tail（forward），以及 mirror 問題上的同一件事（backward）。
2. **unlock-driven list scheduling**（本階段最重要的發現）：
   - 觀察：big2:1,1,1 的 tail-priority 排程，AND 機器前 775 步完全閒置（`shared/tmp/claude/idle.py`）。
     OR 機器照 tail 挑的 OR 湊不齊任何一個 AND 的 fanin，瓶頸機器在開頭「餓死」。
   - 作法：動態維護 `need[a]`＝op a 尚未排程的祖先數。每一步先看 need 最小（≤ 64）的瓶頸 kind op，
     把它的 ready 祖先優先排進去；剩下的 unit 再照 tail priority 填。forward 與 mirror 各跑 4 個變體
     （target = 瓶頸 kind 或全部；被驅動的 kind = 非瓶頸或全部）。
   - big2:1,1,1：8571（tail priority）→ 7518；LB 7512。big3:1,1,1：13500 → 13427 = LB。
3. **forward-backward improvement**：以目前排程的 step 當 priority，交替在 mirror 與原問題上重排，直到連續 4 輪沒有進步。
4. 任何一步達到 LB 就立刻停止。整個 suite 的 `-h` 總時間約 1.3 秒。

`-h` 結果（commit 8f1c0c2 之後）：76 個全部 checker OK，**67/76 的 latency 等於自己的 LB（直接證明最佳）**。
剩下 9 個：C1355:{2,2,1 / 3,2,2 / 8,8,4}、i8:8,8,4、des:3,2,2、big2 四組，差距 1–12。

### 4. Exact（`src/ilp.cpp`）

- **Step formulation 的可行性 ILP**，horizon H = UB−1，逐次遞減：
  - `y_{i,t}=1` 表示 op i 在第 t 步或更早執行；時間窗 `[head_i, H − tail_i]` 用資源感知的 head/tail，窗外固定為 0/1；
  - monotone `y_{i,t} ≤ y_{i,t+1}`；precedence `y_{i,t} ≤ y_{j,t−1}`（每條邊、每個 t，各只有 2 個非零係數，LP 很緊）；
  - resource `Σ_i (y_{i,t} − y_{i,t−1}) ≤ c_k`。
  - Gurobi 回報 infeasible ⇒ UB 是最佳；找到解 ⇒ UB 下降，繼續。
  - heuristic 的用途：UB 決定 horizon，head/tail 決定時間窗（變數數量）；LB==UB 時完全不需要 ILP。
- 小實驗（4 threads）：C1355:8,8,4 h=48 → e=47 optimal（22 s，兩次求解）；C1355:2,2,1 的 178 在 root LP 就 infeasible
  （16 s）⇒ 179 最佳；i8:8,8,4 的 94 infeasible（2.6 s）⇒ 95 最佳；C1355:3,2,2（4 萬變數）root LP 就花 40 s，逾時。
  瓶頸是 root LP（零目標、高度退化的 simplex），不是分支。
- **兩端放鬆 bound（實驗性，`ends_bound`）**：模型超過 40 萬變數時，改解「前 T 步最多能做幾個瓶頸 kind 的 op」的
  prefix ILP（mirror 上再解一次得到 suffix），`L ≥ ceil((N_b + idle_head + idle_tail)/c_b)`。
  推導正確，但目前**沒有效果**：big2 的 head ≤ 23 的 AND 就有 4612 個，prefix 模型 6.5 萬變數，root LP 25 s 內解不完；
  des:3,2,2 解出的 idle 只有 2，bound 仍是 661。保留程式碼（bound 已夾在 `c_b·T` 以內，保證合法），待下一階段改進。

### 5. 正式評估（階段 1 結束時）

（見本檔最後的「階段 1 數字」。）

### 6. 尚未嘗試／下一步的想法

- root LP 太慢：試 `Method=2`（barrier）或 concurrent；用 heuristic 排程當 `VarHintVal`；或加目標函數（min L）取代零目標。
- prefix 模型縮小：只保留可能進入前 T 步的 op（dominance）、粗時間格（checkpoint）版本的放鬆。
- 用 prefix ILP 的解當作 list scheduling 的強制開頭，改善 big2 的 UB（目前 UB−LB = 3–12）。
- CP 式的時間窗傳播（edge-finding／energetic reasoning）在 H=UB−1 時先縮窗，甚至直接證明 infeasible。
- tail 的加強版：對後代做「帶 lag（最長路徑距離）」的 Jackson。
- heuristic：隨機擾動 priority 的多次重啟（時限內的時間可以換 latency）。
- 穩健性：超大輸入時 closure 退回的路徑、`need` 更新的最壞複雜度（closure 大小）需要壓力測試。

## 階段 1 數字（正式評估，`tools/eval.py --mode h,e`，note=`stage1: ...`，commit 見 results.tsv）

- 76 × 2 次執行全部 checker OK；沒有逾時，`-e` 從未比 `-h` 差。
- `-h`：76 個 case 總時間 2.1 s（單一 case 最多 0.2 s）；67/76 的 latency 等於 LB。
- `-e`：**70/76 證明最佳**（67 `bound` + 3 `optimal`），1 `timelimit`，5 `too_large`；總時間 224 s，單一 case 最多 52.2 s。

| case | -h | -e | LB | status | time (s) |
|---|---|---|---|---|---|
| C1355:2,2,1 | 179 | 179 | 179 | optimal | 14.8 |
| C1355:3,2,2 | 141 | 141 | 140 | timelimit | 52.2 |
| C1355:8,8,4 | 48 | 47 | 47 | optimal | 22.1 |
| i8:8,8,4 | 95 | 95 | 95 | optimal | 2.8 |
| des:3,2,2 | 662 | 662 | 661 | too_large | 25.9 |
| big2:1,1,1 | 7518 | 7518 | 7512 | too_large | 25.9 |
| big2:2,2,1 | 3764 | 3764 | 3758 | too_large | 25.9 |
| big2:3,2,2 | 2520 | 2520 | 2508 | too_large | 26.3 |
| big2:8,8,4 | 948 | 948 | 945 | too_large | 25.8 |

`too_large` 的 25 s 都花在沒有成果的 prefix ILP 上（見 4.）。`shared/best.tsv` 目前 70/76 證明最佳。
對照 baseline（main@d8fb11f）：27/76；codex@31d3c14 的 `-e`：60/76。
