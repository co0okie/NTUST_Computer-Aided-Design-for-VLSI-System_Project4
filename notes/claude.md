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

## 自製差異測資：`benchmark/claude_gap.blif`

- 16 個 operation、7 個 PI，資源 1,1,1：`-h` = 9，`-e` = 8（status optimal）；兩個輸出都通過 `tools/check.py`，
  存在 `notes/claude_gap_h.txt`、`notes/claude_gap_e.txt`。重現：
  `build/mlrcs -h benchmark/claude_gap.blif 1 1 1` 與 `-e`。
- 來源：`notes/gen_gap.py search`（隨機 AND/OR/NOT DAG；只有 `-h` 的 latency ≠ LB 時才跑 `-e`）。
  注意：找到之後生成器被改過，`gen SEED ...` 已無法逐位元重建這個檔案，以 commit 的 .blif 為準
  （重跑 `search --seed0 1 --count 4000 --min-ops 8 --max-ops 18`：53 個 latency≠LB，但沒有 e<h）。下一階段要補一個可完全重現的版本。
- 初步分析：三種 kind 各只有 1 台、負載相近，沒有單一瓶頸。heuristic 在第 1 步讓 OR 機器做 n1（tail 最大），
  ILP 的解卻讓 OR 機器第 1 步**閒置**不影響，而把 AND 的順序換成 n0→n7→n2→n5…，使 OR 鏈（n1→n8→n6→n9→n12）
  與 AND 鏈交錯得剛好；heuristic 的 greedy 順序（n0→n5→n7→n10）讓 n2、n3 太晚，後面的 OR（n9、n15、n12）被拖到第 7–9 步。
  也就是：tail priority 只看「後面還有多少工作」，看不到不同 kind 的鏈之間的交錯時機。
- 另外，課程測資 aoi_C1355:8,8,4 也有差異（h=48，e=47）。

## 階段 2（2026-11-11 晚）

### 做了什麼
1. **Gurobi 參數實驗**（subagent；`shared/tmp/claude/exp/`，2 threads）：C1355:3,2,2 的 horizon-140 可行性模型，
   預設／dual／primal／concurrent／barrier 都在 50 s 內沒有結論；只有 `Method=2 Presolve=2` 在 28.5 s 證明 infeasible
   （barrier root 11.5 s，重跑一致）。root LP 時間：barrier 11.4 s，預設 32.9 s。
   prefix 模型（big2、des）則沒有任何參數能在 50 s 內解完 root LP ⇒ 問題在模型大小，不是參數。
2. **`improve_schedule`**（`src/heuristic.cpp`，subagent 實作）：固定 seed 的隨機多次重啟——tail 或目前最佳排程的 step 當 base key，
   加上 0.5–4 個 tail 單位的雜訊，隨機選 list／unlock、forward／mirror，再接 2–4 輪 FBI。
   `-h` 預設用到 10 s（`--htime`）；`-e` 先用 3 s，模型太大（too_large）時把剩下的時間全給它，不再跑沒有效果的 prefix ILP（`use_ends_bound=false`）。
   - des:3,2,2 662→661 = LB（0.04 s 內），C1355:8,8,4 48→47，big2:2,2,1 3764→3763，big2:3,2,2 2520→2518（`-e` 的 52 s 得 2516），big2:8,8,4 948→946。
   - 進步都在前 0.3 s 內出現，之後幾乎停滯（`--htime 40` 只讓 big2:3,2,2 多降 2）⇒ 純隨機重啟已到極限，需要別種 move。
3. 可行性 ILP 改用 `Method=2 Presolve=2`。

### 正式評估（note=`stage2: ...`，commit 55567d1；76×2 全部 checker OK，無逾時，時間無異常——最大 52.03 s）
- `-h`：68/76 等於 LB；未證明的 8 個各跑滿 10 s，總計 81.6 s。
- `-e`：68 bound + 3 optimal（C1355:3,2,2=141 於 37.6 s、C1355:8,8,4=47 於 7.4 s、i8:8,8,4=95 於 21.8 s）= **71/76**；
  1 timelimit（C1355:2,2,1：179，LB 178）；4 too_large（big2：7518/7512、3763/3758、2516/2508、946/945）。
- **退步**：C1355:2,2,1 在階段 1（預設 simplex）15 s 就在 root LP 證明 infeasible，換成 barrier+Presolve=2 後 49 s 逾時。
  兩組設定各擅長一個 case；i8:8,8,4 也從 2.8 s 變 21.8 s。下一步：找單一設定或做 portfolio。
- `shared/best.tsv`：72/76 個 benchmark case 已證明最佳（含階段 1 證明的 C1355:2,2,1）；剩 big2 四組。
