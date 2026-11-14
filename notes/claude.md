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

## 階段 3：big2 的粗時間格（checkpoint）closure 放鬆——負結果

動機：big2 的差距來自 AND 機器開頭／結尾的閒置；per-op head + Jackson 看不到「多個 AND 的祖先集合取聯集」的效應。
模型（`ends_relaxation`，`src/ilp.cpp`，opus subagent 實作）：checkpoint `0=τ_0<…<τ_m=T`，`z_{i,j}`＝op i 在 τ_j 以前完成；
monotone、closure（`z_{i,j} ≤ z_{p,j'}`）、每段容量 `c_k(τ_j−τ_{j−1})`，最大化前 T 步的瓶頸 op 數 M ⇒ `idle_head ≥ c_b·T − M`；
mirror 得 `idle_tail`；`L ≥ ceil((N_b+idle_head+idle_tail)/c_b)`（需 T+T' ≤ LB）。只用 Gurobi 的 proven bound，不用 incumbent。

- 正確性：12 個已知最佳的電路 × 4 組限制 × 3 組 (T,T',m) = 144 組，135 組算出 bound，沒有一個超過最佳值（106 組恰等於最佳），見 `shared/tmp/claude/ck/sanity.tsv`。
- big2 結果：四組的 LB 都沒有提高（7512 / 3758 / 2508 / 945），latency 也沒有降低（7518 / 3763 / 2516 / 946）。
  - 能解完的模型（≲1 萬變數）只證得出「第一個 AND 的 head 之前」那段顯而易見的 idle（1,1,1：10；8,8,4：40）。
    原因：LP relaxation 可以把每個 AND 需要的 OR 各取一部分，看不到聯集效應；整數 bound 要靠 branching，超過 1 萬變數就做不完
    （1,1,1 需要 T≈21，1.6–4.1 萬變數；T=17 每步一個 checkpoint 離線 60 s 也沒解完）。
  - guided schedule（用 z 的解當 priority）：T=7 的精確解顯示 8,8,4 前 7 步可做 16 個 AND（heuristic 只做 3 個），
    但照它排完再做 FBI，latency 沒有變好——前段搶快，後段就缺 AND 可做。⇒ **開頭的閒置不是 big2 差距的全部**，瓶頸可能在中後段的供料。
- 處置：程式保留、預設成本很低（head 模型 > 1.5 萬變數直接跳過；合計上限 10 s），測試用 flag `--ends`、`--ends-test`、`--ends-time`。
- 結構觀察（留給之後的 heuristic）：big2 前段的 AND 大多需要約 10 個 OR，每組只有 2–3 個 AND 共用同一組 OR（set-union knapsack）。
  `unlock_schedule` 的 need 最小貪婪已是「聯集增量最小」；要更好需要「一組 OR 一次解鎖幾個 AND」的比例式選擇。

## 階段 4：自製難測資 suite（`benchmark/hard/`）

動機（AGENTS.md「heuristic 優先」）：benchmark 幾乎全是資源主導，`-h` 已貼著 LB，分不出 heuristic 的好壞。
做法（sonnet subagent；凍結 binary＝55567d1）：`benchmark/hard/gen_hard.py` 四個 family——
`layered`（每層三種 kind 混合的隨機分層 DAG）、`chains`（kind 樣式錯相的長鏈＋晚釋放的側邊邏輯）、
`funnel`（一種 kind 的 feeder 餵給重疊的 consumer 群）、`xor`（parity/adder/multiplier，對照組）。
搜尋約 1,800 組（電路 × 限制），約四分之一 `-h --htime 0` 高於 LB；約 260 組離線跑 `-e`（30–300 s）。
流程腳本 `search.py`、`build_manifest.py` 一併保留；原始資料在 `shared/tmp/claude/hard/`。

- 成品：47 個 `.blif`、50 個 case（80–4800 ops），`manifest.tsv` 記錄生成參數、h0/h/best/LB；**42 個已證明最佳**，8 個 open。
  `gen_hard.py --all --out DIR` 重新生成後 47 檔逐位元相同；manifest 與 `shared/best.tsv`（50 列，note `hard suite`）一致（獨立 subagent 驗證）。
- 什麼讓 case 變難：規模 250–2500 ops（更小的 random restart 幾乎都能找到最佳）；layered 每層 kind 比例不同、
  且 `ceil(N_k/c_k)` 約為深度的 0.6–1.2 倍時最容易騙過 heuristic；xor 類從來不難（4 個 case 全部 h=best）。

### 正式評估（note=`stage4: ...`，commit 6111541；252 列全部 checker OK，無逾時，最大 53.09 s）
- 難測資 `-h`：`latency/best` 幾何平均 **1.00459**（50 個）、1.00485（42 個已證明）；36/50 等於 best；14 個各差 1 step
  （layered 7、chains 4、funnel 3；例：`layered_s129_n252:5,3,2` 31 vs 30）。`--htime 0` 的平均約 1.03（46/50 高於 best）。
- 難測資 `-e`：39/50 證明（optimal 或 bound）；48/50 等於 best；2 個 timelimit 高於 best
  （`layered_s1074_n757:5,4,2` 88 vs 87、`layered_s554_n1785:11,19,9` 68 vs 67——離線較長時間才找到）。沒有任何結果低於 best、沒有 LB 高於 best。
- 預設 suite：`-h` 68/76 等於 LB；`-e` **72/76** 證明（C1355:2,2,1 這次也證出來了，階段 2 是 timelimit ⇒ 該 case 在時限邊緣，不穩定）；
  未證明只剩 big2 四組（7518/7512、3763/3758、2516/2508、946/945，皆 too_large）。

### 自製差異測資失效
`benchmark/claude_gap.blif:1,1,1`：現在 `-h`＝8＝`-e`（bound），只有 `--htime 0` 是 9。random restart 把它解掉了，
**不再符合「-h 與 -e 不同」**。替代候選：難測資中 `-h` 高於已證明最佳、且 `-e` 在 60 s 內證明的 case，
最小的是 `layered_s129_n252:5,3,2`（252 ops，`-h` 31、`-e` 30 optimal，6.1 s），可重現（gen_hard.py）。
缺點是 252 ops 太大，報告裡不好分析成因；想要小的反例，要對「含 restart 的 `-h`」重新搜尋。

### 未嘗試的想法（更新）
- heuristic 在難測資上的 14 個差 1：local repair／sliding-window 重排（取代純 random restart）、CP 式時間窗傳播判定 L−1 不可行區段。
- big2：比例式 unlock（一組 OR 解鎖幾個 AND）、lag-aware tail。
- 穩健性壓力測試（超深、超寬、高 fanout、closure 超過 400 MB 的 fallback）尚未做。

## 階段 5：deterministic `-h`（工作量預算）＋目標 latency DFS＋plateau 局部搜尋；穩健性壓力測試

### 「72 → 68」不是退步
55567d1 與 6111541 的 `-h` 在預設 suite 76 個 case 上 latency 完全相同。72 是「等於 best known」，68 是「等於自己算出的 LB」；
差的 4 個（C1355 ×3、i8:8,8,4）是 LB 不緊，`-h` 已是 `-e` 證明過的最佳。但重啟次數原本由 wall clock 決定，負載下可能不同 ⇒ 改成 deterministic。

### heuristic（opus subagent，commit 91cf561；評估列 7fe920a，note=`stage5: ...`）
診斷：14 個差 1 的 case 有 12 個最佳＝LB，隨機重啟找不到「瓶頸完全不閒置」的解。
- 捨棄：pilot/rollout（比 unlock 起點還差）；squeaky wheel（4 種 gain 都沒改進起始解）；DFS 加 restart／失敗加權（丟掉 funnel_s1210）；
  祖先數量推 est（慢 4 倍無新解）；mirror DFS（無新解）。
- 採用 1：**目標 latency 的 DFS**——固定 L−1，逐步選 op（只試把機器填滿的選法），每步用 precedence 推 est、每種 kind 各跑 EDF 檢查可行性。
  解開 3 個 funnel（511／3.7k／62k 節點）；layered、chains 給 100 萬節點也解不開。搜尋走完無解 ⇒ **證明 L−1 不可行，LB 提高**
  （C1355:3,2,2 在 0.37 s 得到 bound，原本要 Gurobi 37 s）。
- 採用 2：**plateau 局部搜尋**——擾動目前排程的 step 順序＋2 輪 FBI，latency 不變差就接受（同 latency 的解之間隨機遊走），候選以 excess 分高下。
  修好 chains ×3、funnel ×2、layered_s1008 ×2。（改成 excess 不變差才接受只修好 5 個。）
- 保留舊的 random restart（30% 預算），否則 layered_s554 退步成 69。
- 流程：初始排程（不變）→ DFS 20% → restart 30% → 局部搜尋。seed 固定。
- **預算**：`min(2e8, 3e5 × ops)` 工作單位（本機約 50 ns／單位）；一次 list scheduling 計 n（密圖 `max(n, n/2+e/8)`），DFS 節點計 n/4。
  wall clock 只當保險（heuristic 20 s、improve `min(30, time−20)`）。`--heffort F` 縮放預算，取代 `--htime`。
- `-e` 先跑與 `-h` 完全相同的 heuristic 再進 ILP ⇒ `-e` ≤ `-h` 由結構保證。新增 `IlpOptions.max_nonzeros = 16M`（防 dense 模型吃 2.7 GB）。

### 正式評估（commit 91cf561，全部 checker OK）
- 難測資 `-h`：幾何平均 1.00459 → **1.00229**（以更新後的 best 計；44/50 等於 best）。修好 8 個；新紀錄 `chains_s579_n2453:16,9,3` 127→126（manifest 已更新）。
  仍差 1：chains_s575 113/112、layered_s129 31/30、s1076 48/47、s1006 49/48、s557 63/62、s554 68/67。時間：中位數 2.9 s、p90 10.6 s、最大 16.1 s。
- 預設 suite `-h`：無退步，69/76 等於 LB，最大 11.4 s。big2 新紀錄：7518→**7517**、3763→**3761**、2516→**2514**（8,8,4 仍 946）。
- `-e` 回歸（9 個 case，≤52.1 s）：C1355:2,2,1 179 optimal（51.3 s，仍貼著時限）、3,2,2 141 bound 0.4 s、8,8,4 47 optimal 9.7 s、i8:8,8,4 95 optimal 23.8 s、
  des:3,2,2 661 bound、layered_s129 30 optimal 6.1 s；layered_s1074 87、chains_s1101 90、big2:3,2,2 2514 未證明。每個都 `-e` ≤ `-h`。
- 確定性：6 個 case 閒置兩次＋同核心 3 個忙迴圈一次，輸出逐位元組相同。**注意**：負載下 layered_s557 跑到 30.0 s 剛好碰到保險；
  機器慢約 3 倍以上時保險會觸發，結果就不再保證相同（仍合法）。
- 完整 76 case 的 `-e` 還沒在 91cf561 上重跑。

### 穩健性壓力測試（sonnet subagent，對象 f275c81；`benchmark/stress/gen_stress.py`、`run_stress.py`、`fastcheck.py`）
約 400 次執行：無 crash、無 checker 判錯、無逾時；`-h` 在 1e6 ops（chain／flat／tree／star／layered）1–1.7 s、RSS 310–515 MB。
ASan 在本機連結不起來（缺 runtime），只用 UBSan-trap＋`_GLIBCXX_ASSERTIONS` 測到 2 萬 ops；heap 錯誤未驗證。
發現：(1) `dense 3000 50`:1,1,1 `-e` 1546 > `-h` 1545（已由 91cf561 結構性修掉：兩者 1544）；(2) 同 case `-e` 2.69 GB（已加 nonzero 上限，45 MB）；
(3) 限制 2147483647 被拒（上限 1e9）——待修；(4) 目錄當 BLIF 會輸出 LATENCY 0——待修；(5) `tools/check.py` 平方時間——已修（2e119fa）。

## 階段 6：目標 latency 搜尋加時間窗 propagation；輸入修正；差異測資

### 輸入修正與回歸（sonnet subagent；b070573、8731845、c2d5487）
- 資源限制接受到 2147483647（讀完 BLIF 後夾到 `max(1, 該 kind 的 op 數)`，不影響結果）；目錄當 BLIF、`--time inf/nan` 為乾淨錯誤。空檔案仍輸出 `LATENCY: 0`。
- 壓力測試重跑（含 1e6 ops）：191 列無 crash／逾時，無 `-e` > `-h`；`-h` 最長 8.43 s、RSS 最大 515 MB。
- 預設 suite `-e`（b070573）：76 全 OK，72 證明（69 bound + 3 optimal），big2 ×4 too_large，最大 52.03 s。

### heuristic 第二輪（opus subagent；7aa53af、df7554f；評估列 dc98e0e）
診斷 `layered_s129_n252:5,3,2`（AND 123/5、OR 75/3、NOT 54/2，深度 21，最佳 30）：NOT 最緊，30 step 內 NOT 只能閒置 6 個 slot，
heuristic 解閒置 8 個；head/tail 時間窗 slack 全 ≥2（沒有 op 被固定）。舊 DFS 24 萬節點最深只到 step 11，失敗根源在 step 1–4：
固定最佳解前 4 step 後 4,901 節點就解出，只固定前 2 step 則 3 萬節點解不出 ⇒ propagation 太弱＋chronological backtracking 回不到早期選擇。
- 採用：每個 open op 維護 `[est, lst]`，precedence 雙向＋每個 kind 的 Hall 區間檢查（過滿即失敗；剛好滿時把其他 op 推出區間）推到 fixpoint；
  **隨機重啟**（重啟 0 不加擾動、完整搜尋，走完無解仍是 L−1 不可行的證明；之後每次擾動分支順序、上限 1000 節點）；**forward／mirror 交替**。
  預算切法：第一段 forward 佔 DFS 階段的 40%，之後每段 1/8。總預算、seed、確定性不變。
- 負結果：LDS（discrepancy 0–5）；祖先／後代數量推 est/lst（無效且慢 6 倍）；從最深 prefix 或 incumbent prefix 重啟；
  每決定一個 kind 就 propagate；調 DFS 佔比（0.2/0.35/0.4 相同）。DFS 佔 90%＋預算 ×4 只多解出 layered_s1006 ⇒ 其餘在現行預算下無望。
- soundness 修正：lst 變成動態後，backtrack 會殘留較深節點的值 ⇒ 同一 step 的選擇改用該 step 開始時的 lst 快照。
- 結果（df7554f，全部 checker OK，無退步）：
  - 難測資 `-h` 幾何平均 1.00229 → **1.00145**，46/50 等於 best（新解 layered_s129 30、chains_s575 112——後者只能從 mirror 方向解）。
    時間中位數 2.8 s、p90 9.9 s、最大 10.6 s（原 16.1 s）。
  - 預設 suite `-h`：latency 不變；等於自己 LB 的 69 → **71**（C1355:2,2,1 在 0.7 s 證出 179、i8:8,8,4 在 1.0 s 證出 95——不需要 Gurobi）；最大 8.9 s。
  - 驗證集（30 個新生成的 layered，200–2000 ops）：latency 總和 1599 → 1598（改善 1、變差 0），6 個新證明 L−1 不可行。提升很小 ⇒ 沒有過度擬合，但也代表方法只對特定結構有效。
  - 確定性：4 個 case 閒置 ×2＋負載 ×1 逐位元相同；負載下 chains_s575 29.3 s，貼著 30 s 保險。
- 仍未解：layered_s1076 48/47、s1006 49/48、s557 63/62、s554 68/67。forward 卡在後段某 kind 供料不足（s1076：OR 在 [29,47] 需要 156 slot 只有 152），
  mirror 卡在前段。需要跨 kind 的推理（「kind k 要在 a 之前完成 D 個 op，需要多少祖先產能」）。

### 差異測資
- `benchmark/claude_gap2.blif`（53 ops funnel，2,4,1；b070573 時 `-h` 11、`-e` 10）在 df7554f 又被 `-h` 解掉（10）。
  搜尋統計（b070573）：約 6.3 萬組候選，20–39 ops 約 1.7 萬組沒有任何 `-e` < `-h`；`-e` 贏的只有 10 組。
- heuristic 每變強一次，差異測資就失效一次；最終版的差異測資必須在 heuristic 凍結後重新搜尋（進行中）。

### 階段 6 驗證（sonnet subagent；df7554f 的快照；評估列 7aa7000，差異測資 407d1d4）
- **新 LB 的正確性**：34,117 組小電路（10–900 ops，全 family，含 8.8k 組 1,1,1），以 b070573 的 `-e` 當獨立參考跑 290 次（全部 optimal/bound）：
  沒有任何 `-h` LB 高於真實排程、沒有 checker 失敗。10–25 ops 的 12,792 組 `-h` 全部自己證明最佳。
- **差異測資**：`benchmark/claude_gap3.blif`（layered，180 ops，深度 18，71 AND／72 OR／37 NOT），限制 4,3,2：`-h` 25（LB 24，2.0 s）、`-e` 24 optimal（2.7 s）。
  重現：`python3 benchmark/hard/gen_hard.py layered seed=206796 layers=18 width=10 profile=uniform -o benchmark/claude_gap3.blif`（檔名會進 `.model`，要同名才逐位元相同）。
  10–150 ops 的 33,413 組候選沒有任何差異 case；151–300 只掃了 553 組就找到這一個。`claude_gap2` 已移除。
  注意：這個 case 上 b070573 的 `-h` 反而找得到 24——新搜尋在這裡比舊的 chronological DFS 差。
- **新版比舊版弱的地方**（非 soundness）：除 gap3 的 latency 外，3 個 case 的 LB 少 1（舊 DFS 走完證明了 L−1 不可行，新版沒證出）：
  `gap seed=946646 nops=84 nin=7`:2,1,2（19 vs 20）、`layered seed=466475 layers=13 width=11 profile=uniform`:6,7,3（14 vs 15）、
  `layered seed=369341 layers=14 width=20 profile=hourglass`:4,6,5（16 vs 17）。⇒ 下一步：把舊的 chronological DFS 放回 portfolio。
- 預設 suite `-e`：76 全 OK，72 證明，big2 ×4 too_large（7517/7512、3761/3758、2514/2508、946/945），最大 52.05 s。
- 難測資 `-e`：50 全 OK，40 證明（階段 4 是 39；新增 layered_s557 62 optimal）；只有 layered_s554（68 vs best 67）高於 best，最大 53.16 s。
- 壓力測試（含 1e6 ops）：191 列，無非預期結果；`-h` 最長 8.11 s、RSS 515 MB、`-e` 最長 52.10 s，無 `-e` > `-h`。
