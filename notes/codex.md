# Codex 研究日誌

## 2026-11-11：起點與第一輪研究假設

已完整閱讀 AGENTS.md、docs/Project04/Project04.md 與 docs/BLIF/BLIF.md。起點 b91a48b；claude、human 當時也在相同 commit，沒有可採用的其他路線改進。研究遵守 C++17、CLI Gurobi、60 秒總時限與輸出前 validate。

### 動機與選擇

Baseline 的資源工作量 bound 假設所有操作可從 step 1 到最後一步使用任意資源，但實際 DAG 有起始延遲與尾端路徑。先隔離研究 lower bound，不修改 heuristic，避免無法區分是上界還是下界改善。

對候選 horizon H，令 r_i = ASAP(i)，q_i 為 i 後方最長路徑的 operation 數，d_i = H-q_i。任何合法排程均滿足 r_i <= t_i <= d_i。逐種類移除 operation 間 precedence，只保留這些時間窗與每 cycle 容量，得到 unit-job 放鬆問題。任一種類不可行則原問題不可行。

按 release time 加入任務，每一步選 deadline 最早的至多容量個任務（EDF）。Unit-job 的交換論證：若可行解在該時段安排其他已 release 任務，交換較早 deadline 任務與其較晚位置不會破壞 release/deadline；因此 EDF 失敗確實代表放鬆不可行。H 增加只放寬 deadline，故可二分搜尋最小可行 horizon。排序一次，每次 heap 掃描 O(n log n)，總 O(n log² n + m)，O(n+m) 空間；不建立 operation × horizon 矩陣。

Baseline 對照：全 76 case 的 heuristic，以 tools/eval.py clean build、CPU 4–7 與 checker 驗證並保存到 codex-results.tsv。實作交由 subagent，主導者負責模型推導、審查及實驗統整。

### 穩健性審查

原 exact_schedule 將 run_gurobi 錯誤直接傳回，並無條件採納 solver 解；這違反中斷時保留合法 heuristic、-e 不比 -h 差的要求。另 LP 字串與 active 索引可佔大量 RAM，建立模型階段沒有時間檢查。獨立修補 fallback 與模型建立上限，保持 ILP 數學限制不變，以免混淆第一輪 bound 實驗。

### 待研究

- 時間窗 bound 後剩餘 gap 的來源：跨種類 precedence 與 join 同步。
- 固定 horizon feasibility ILP 與累積 precedence formulation：更緊的 relaxation 是否抵消列數增加？
- 利用前驅/後繼集合的資源工作量加強每個 node 的 release/tail（需限制 transitive closure 記憶體）。
- heuristic 多種 deterministic priority 或 backward scheduling，待 lower bound 實驗完成再獨立比較。

### 第一輪實驗結果

- 獨立 subset-DP 最佳解 oracle：19,267 組，包含 <=4 node 的 ordered DAG / operation type 組合與 3,000 個隨機 DAG，及空圖、未使用種類容量 0、INT_MAX 容量。沒有 lower bound 超過 optimum，也沒有弱於舊 bound；1,164 組提升、19,252 組 tight。可重跑程式 notes/check_window_bound.cpp。
- 全 76 benchmark 的 heuristic latency 完全不變，checker 全數 OK。時間窗 bound 改善 49 個 case；bound 等於 heuristic 的 case 從 27 增至 57（+30）。eval.py 記錄的 runtime 加總 0.72 → 0.84 秒，clean build 12.3 → 12.5 秒。runtime 每列只記錄到 0.01 秒，總量很小，不應過度解讀差異。
- 例：aoi_t481:1,1,1 的新 bound=883，直接等於 heuristic；aoi_big3:3,2,2 的新 bound=6718，亦直接證明最佳。這些 case 因而不再需要建立大型 ILP。
- 自製 benchmark/codex_gap.blif 只有 4 operation：NOT n0、n1；OR n2 依賴兩者，OR n3 只依賴 n1 與 PI。n0/n1 的 height 同為 2，檔案順序先排 n0，得到 latency 4；先排 n1 可排 n1 / n0+n3 / n2，latency 3。兩個 NOT 均須先於 n2，證明 optimum >=3；Gurobi 證明 3。生成方式 notes/generate_codex_gap.py。
- exact fallback fault injection 11 種（非零退出、SIGTERM、缺解、缺 operation、fractional、多個 one-hot、資源違規、infeasible 聲明、合法 interrupted incumbent、強制 SIGKILL、0.01 秒預算）均回傳 checker-valid heuristic。真 Gurobi 在 codex_gap 得到 optimal=3。
- model 建立加上 1,000,000 binary 與 128 MiB LP text 上限、剩餘 4 秒停止條件，並在啟動 solver 前釋放 LP 與 active buffers。這是保守的工程預算，不宣稱能嚴格限制 Gurobi 的 peak RAM；大型 case 仍可能因模型上限回傳 heuristic。

### 結論與下一步

低成本時間窗放鬆對課程 benchmark 很有效，先前大型模型困難的一大部分其實可由下界直接處理。接著針對其餘 19 case，測試用祖先/後繼集合的資源工作量加強時間窗；同時以 exact 子集正式評估確認 bound status 與 fallback。完整 exact suite 尚未跑，不能將 57 稱為全 suite 實測 exact optimal 數（目前是經 checker 驗證的 heuristic 與已驗證 bound 相等數）。

### 第一輪 exact 子集（6 case）

全部 checker OK：cm138a:1,1,1（10）、x2:2,2,1（14）、t481:1,1,1（883）均 `bound`；codex_gap 為 `optimal=3`。C1355:2,2,1 為 `timelimit`，保留 185，LB=167，51.39 秒；big3:8,8,4 為 `too_large`，保留 1692，LB=1687，0.07 秒。總 runtime 51.54 秒。這支持第一輪能快速避開已證明 case，但沒有解決剩餘大型 ILP gap。

## 第二輪：集合工作量與單次 EDF

動機：4-node gap 的兩個 NOT 必須同時先於 join；單純路徑 early 忽略 join 的共同祖先工作量。對每個 node i，祖先種類 k 的數量 A_ik 給出 early_i >= 1+ceil(A_ik/R_k)；後繼集合對稱給出 tail_i >= ceil(D_ik/R_k)。每個集合只計一次共享祖先，不能簡單加總前驅的 bound。以 operation-index bitset 計算 closure，單方向矩陣重用；設 64 MiB 與估算 word work 上限，超過時回退第一輪時間窗，避免最壞情況超出預算。

在此輪實作期間發現 claude 已提交 `9e7a4cd`，含 recursive resource heads/tails 與 Jackson bound。其模型提醒我們：所有 deadline 都是 H-tail，EDF 的順序其實不依賴 H；因此第一輪二分搜尋可改為一次 EDF，取 max(t+tail)，得到相同 bound。此簡化的靈感來源明確記為 claude@9e7a4cd；實作由 codex 自行改寫，不複製其程式。此輪保留較簡單的 closure count 作獨立 ablation，與 claude recursive tail 方法區分。

### 第二輪初步結果

closure-count + single-pass EDF 全 76 heuristic case checker OK、latency 不變。相較第一輪，8 個 bound 再提升，直接證明數 57→59：新增 i8:2,2,1（371）、C6288:1,1,1（1622）。總 runtime 0.84→1.21 秒，clean build 12.9 秒。相較 baseline，依然只改善 bound，不應宣稱 heuristic 品質變好。

獨立 exact-DP 19,267 case 再次全部通過，並加入反轉 node ID 的不變性檢查；tight 從 19,252 增到 19,263。另驗證 65/129 node bitset 跨 word、25,000 node 的記憶體 cap fallback、6,000 node width-300 密集 DAG 的工作量 cap fallback。15,000 operation 的矩陣及種類遮罩約 26.90 MiB，node index 另約 0.12 MiB；兩個方向重用同一矩陣。實際 word 工作量有雙向與種類計數常數倍，100M 門檻是預算估算，非硬性 wall-clock 保證。

重現故障測試已保存 notes/check_exact_fallback.py，可用 --binary 指定編譯後程式、--gurobi 指定真 solver（可選）。15 項包含故障回退、較佳 incumbent、optimal bound 一致性、缺 solver 非零退出與真實 solver 均通過。

報告素材：notes/summarize_codex.py 由 eval TSV 生成 codex-summary.tsv 與 codex-bound-ablation.svg。h/LB 幾何平均是最優性 gap 的上界指標，並非研究規範中的 h/best-known，兩者不可混用。

### 後續 ILP 實驗設計（尚未實作）

現有 precedence 只有期望 start(v)-start(u)>=1：fractional x 可把一部分 mass 提前、另一部分拖到很晚，平均時間合法但不代表每個閾值都符合 precedence。候選替代模型為 cumulative z_it=1 iff operation i 已在 t 結束。單調性 z_i,t-1<=z_it；precedence z_vt<=z_u,t-1；每種資源 sum_i(z_it-z_i,t-1)<=R_k。窗外 z=0/1 視為常數，減少變數與係數；整數可行排程與原模型等價。此模型在 relaxation 上蘊含目前期望 start precedence，但反向不成立（可用 fractional witness 設計動機實驗）。

先做固定 horizon H=heuristic-1 feasibility：不可行即證明 heuristic 最佳，可行則解碼更短 schedule，再降低 H。風險：失去原 heuristic 的可行 MIP start，較多 precedence rows，且若只嘗試 H=UB-1 未成功無法提升全域 LB。應先在 C1355 四組資源上分別測量 LP relaxation、rows/nonzeros、solve time，再決定是否整合；大圖不直接套用，以免重演 presolve 爆炸。亦可在 UB horizon 上保留優化 makespan 模式做對照。

因 claude 的新方向已包含更強 resource tails 與 heuristic unlock priority，codex 下一輪優先保持自己的 baseline heuristic 作 ILP ablation，避免兩路同時重複調 heuristic。

### 第二輪完整 exact suite 與研究指標

完整 76-case `-e` 全數 checker OK（clean build 12.5 秒），59 `bound` + 1 `optimal` = **60/76 證明最佳**，4 `timelimit`、12 `too_large`，總 runtime **227.37 秒**。C1355:8,8,4 從 heuristic 50 改善至 48，但未證明最佳（LB=46）；i8:8,8,4 於 16.71 秒證明 optimum=95，已提高 shared/best.tsv 的 lower bound。其餘 C1355 三組逾時保留合法 heuristic。所有 case -e <= -h，沒有 checker failure 或外部 60 秒 timeout。

資料版本：第二輪 eval 列標 `b503259+dirty`，其 source snapshot 與後續 commit `31d3c14` 相同（僅 header 說明文字時點可能不同）；完整 e 在評估中提交 source，因此 TSV 不改寫歷史 commit 字串。

以 2026-11-11 15:21:48 PST 的 shared/best.tsv 為比較基準（含 claude@b522503 的改進），規範指標如下：

| 指標 | 數值 |
|---|---:|
| -h 全 76 case latency/best-known 幾何平均 | 1.007498 |
| -h 總 runtime | 1.21 s |
| -e optimal 或 bound | 60/76 |
| -e 未證明 16 case latency/best-known 幾何平均 | 1.033480 |
| -e 總 runtime | 227.37 s |

原始結果 codex-results.tsv，指標 codex-metrics.json，比較基準快照 codex-best-snapshot.tsv。best-known 是三路持續改變的紀錄，因此需要保存快照才能重現比例。Baseline、EDF、closure 三種的 runtime 比較來自同一工具、相同 case/資源；不能將其他路線後來的 best-known 當成 baseline 當時已知值。

### 下一輪交接

先讀 shared/questions.md 的 Q4 與 claude@b522503：對方 heuristic+recursive tail 已直接證明 67/76，big2/big3 改善遠大於本輪，應保留其成果供後續整合。但 codex 此輪已完成獨立的下界 ablation、robust fallback、完整 exact 實測與 tiny oracle，不要抹除這些對照數據。

下一輪優先在 C1355 上試 fixed-horizon cumulative precedence ILP，先測 LP relaxation 與模型 nonzeros，避免只把完整 suite 再跑一次。若採用 claude heuristic，另開清楚的對照組，並重新尋找/確認自製 h/e 差異測資仍成立。提交用 3 頁 PDF 尚未製作，現有 figures/table/gap 分析已備妥，可待演算法收斂後排版。

## 主持人恢復後的有界階段：累積 precedence 對照

已合併 main@7344de9（merge b376c9b），開始時工作目錄乾淨。接受更新後目標：只有 latency 要最小化；runtime 只作 60 秒硬限制及報告資料，不將微小 runtime 差異當作改善目標。BLIF 維持純 AND/OR/NOT，未開啟其他邏輯支援。已讀 shared/questions.md，截至 Q5 無待回答問題。

本輪限定上一輪已規劃的 cumulative precedence 實驗，先隔離單一因素：沿用原 time-indexed x、latency objective、資源限制、MIP start、上界及 fallback；只把平均 start precedence 換成每個時間閾值的 cumulative inequality。暫不改固定 horizon、不整合其他 heuristic，也不跑完整 suite。

對 operation edge u→v，所有 t 施加 sum_{s<=t} x_vs <= sum_{s<=t-1} x_us。整數 one-hot 下，若 v 在 t 開始，u 必須已在 t-1 或更早開始，故與原 precedence 等價。加總閾值可推出平均 start 不等式，因此 relaxation 不弱於原模型；但 x-prefix 直接展開會增加到 O(edges × window²) 個 terms，可能先撞到 128 MiB 建模上限。這個規模代價也是本輪要量測的結果，不能只因 relaxation 較緊就宣稱改善。

限定 C1355:2,2,1、C1355:8,8,4 與自製 gap，各跑 scalar control 與 cumulative variant，以 tools/eval.py、相同 CPU 4–7 / Gurobi 4 threads、checker 驗證並保存所有結果。實驗由單一 subagent 執行，正式 src 保持不變；沒有實測 latency 收益就不替換 production 模型。

### 累積 prefix 階段收尾（重啟後檢查）

六個背景 run 已於 2026-11-11 15:42–15:45 完成，無需重跑，所有 eval 記錄皆為 checker OK：

| case | scalar latency/status | prefix cumulative latency/status |
|---|---|---|
| C1355:2,2,1 | 185 / timelimit | 185 / too_large |
| C1355:8,8,4 | 48 / timelimit | 49 / timelimit |
| codex_gap:1,1,1 | 3 / optimal | 3 / optimal |

C1355:8,8,4 的 binary 數同為 14,057，但 LP bytes 999,315→5,229,466，constraint rows 1,804→20,543；C1355:2,2,1 prefix 在 128 MiB LP 上限前中止，因此沒有完整模型大小記錄。正式模型不採納這個變更：本次 prefix 沒有降低 latency，且一組甚至無法進入 solver。這是單次對照的負結果，不宣稱所有 cumulative formulation 都差，也未量測 root LP relaxation 值。

可重現腳本、來源核對與完整表見 cumulative_ablation.py、cumulative-ablation.md、cumulative-model-size.tsv。TSV 的 commit=7344de9 是 eval 在 shared symlink 實際路徑讀到 main 的 metadata；實際 snapshot 已逐檔與 b376c9b 及轉換腳本核對一致。六個正式輸出在當時由 tools/eval.py 呼叫 check.py 通過後刪除，現存 cumulative-gap.out 已另外重驗通過；不把不存在的原始輸出冒充本次重新驗證。

shared/questions.md Q6 已讀：claude@ce1d6e4 使用緊的 resource heads/tails 及 horizon=UB-1 step feasibility，達 70/76；對方也指出 C1355:3,2,2 root LP 退化問題。本輪沒有待回答的 open 問題。

## 主持人追加授權的一個階段：精簡 cumulative 變數

沿相同累積 precedence 方向，將反覆展開的 prefix sum 換成單一 z_it 變數。保留原 ASAP/ALAP(heuristic latency)、minimize L、heuristic 上界與 MIP start，讓模型表示方式成為主要變因。z_it=1 iff start_i<=t，窗外視為 0/1 常數；用 monotone、z_vt<=z_u,t-1、sum(z_it-z_i,t-1)<=R_k。由 start_i=late_i-sum_{t=early_i}^{late_i-1} z_it，sink 的 L+sum z_it>=late_i。每列 precedence 最多兩個非零係數，避免 prefix 展開的二次增長。

此處與 claude Q6 共用 step formulation 的數學想法，來源已註明，但本次保留 minimize L 與原時間窗作受控對照，沒有複製對方程式，也不另試 fixed horizon/solver parameters。實驗仍僅兩個 C1355 與 gap，另用小 DAG 的獨立 exact oracle 驗證；正式 src 等待證據，不在此階段擴大整合。

### 精簡 cumulative 階段結果與停止點

獨立 subset-DP / check.py 共 31 個 tiny case 全通過；其中 20 個 status=bound，11 個 status=optimal（gap 加上特別挑選的 10 個 h>opt 小圖，確保實際建模求解），不把 bound case 計為 model 解碼驗證。輸入及輸出保留於 shared/tmp/codex/compact-validation，生成與驗證腳本 notes/check_compact.py。

三個正式 run 全 checker OK，clean build 13.2 秒：

| case | scalar control | prefix cumulative | compact cumulative |
|---|---:|---:|---:|
| C1355:2,2,1 | 185（52.40s） | 185（too_large，0.72s） | 185（timelimit，51.52s） |
| C1355:8,8,4 | 48（51.15s） | 49（52.85s） | 50（timelimit，51.16s） |
| codex_gap:1,1,1 | 3 optimal | 3 optimal | 3 optimal（0.02s） |

Compact C1355:2,2,1 可建模（100,219 binary、256,898 rows、13,001,702 LP bytes），避免直接 prefix 展開的文字大小 guard；但求解品質仍未提升。所有數字都來自同資源、CPU4–7、Threads4 的既有 control / 新 variant，各僅一次，不宣稱穩定統計差異。背景環境與 wall-clock 結束點仍可能影響求解軌跡。此階段未保留完整 C1355 solver trace，也未量測 root relaxation，無法把失敗直接歸因 presolve 或 simplex；可以確定的是 compact encoding 本身不足以在這組對照降低 latency。

決定：**不採納到 production src**。理由是目標 latency 無收益，而非 runtime 大小；若未來證明較慢但 latency 更好且 <60 秒，仍可採用。精簡模型保存為 notes/compact_ilp.cpp，配合固定 base b376c9b 的 compact_ablation.py、source manifest、模型尺寸表及 compact-ablation.md 可重現。實際 source 與 TSV 的 shared symlink metadata 問題同上，以 manifest 為準。

shared/best.tsv：預設 suite 沒有任何數值改善，保留其他路線更好的紀錄；本輪補登自製 gap optimum=3（在 default76之外），追加精簡模型的驗證來源，不把負結果改寫成新的 best。Q6 已讀、Q7 已告知 prefix 負結果，精簡模型結果另追加通知。

未完成：full-suite compact 評估、root LP 診斷、正式整合及 3 頁報告，均不在本輪有限階段內，沒有啟動。下一步若主持人再授權，先依 claude Q6 對照緊時間窗/UB-1 formulation 與目前寬窗 min-L 的模型大小及 root 行為，聚焦尚未解的 C1355:3,2,2；不再盲目重跑這兩種寬窗 cumulative 表示法。此處按主持人要求停止。

## 時間窗 × horizon 診斷階段（主持人指定）

動手前已讀 claude@55567d1：在既有 UB-1 feasibility 中使用 Method=2、Presolve=2（保留 DualReductions=0），已證明 C1355:3,2,2 optimum=141。本輪不重做這個搜尋。shared Q6/Q8 已讀，無待答 open 問題。

限定 C1355:2,2,1 的 2×2 controlled diagnostic：path ASAP/ALAP 與 recursive-resource 時間窗；原 UB 的 min-L 與單次 H=UB-1 feasibility。固定本路線原 heuristic、Method=2、Presolve=2、DualReductions=0、Threads=4；計時只用 CPU4–7。另用 codex_gap 及小型 DAG 驗證。這個新 min-L/path control 也採相同 barrier/presolve 設定，因此不能把其差異全部歸因時間窗，更不能與上階段 auto-method runtime 直接當單因素比較。

強窗計算採用 claude@55567d1 的 bounds/opgraph 檔案，固定 commit 並明確註記來源；以既有 compact 模型為基礎，不修改正式 src。所有實驗保留原始 schedule、stderr、LP、solution 與 Gurobi log，由 diagnostic harness 呼叫 check.py；對每個 run 記錄模型大小、presolve/root LP 時間與分支節點，未出現的階段記為未觀測。此輪不搜尋 solver parameters、不開新 circuit suite。

單次 feasibility 的 optimal status 只代表零目標問題解完，不能當成原排程最佳。只有 H=UB-1 確定 infeasible 才能證明原 UB 最佳；或合法 incumbent 等於有效 LB。由於 heuristic 原本長 UB，H=UB-1 不提供不可行的 heuristic MIP start，這是與 min-L 控制組的內在差異，必須在結論揭露。

進行中：只授權一個 subagent 完成實作、驗證與診斷；若額度降至 10% 以下，本輪依主持人最新指示留下可恢復 notes/commit 後停止，不等待後另開研究。

### 2×2 診斷結果：本階段完成

Clean build 6.80 秒（首次建立的新目錄）；48 個小型驗證全部通過獨立 DP/checker：10 個 h>OPT 小圖 ×4 模型、4 個強制建模 infeasibility 控制與4個 gap preflight。正式診斷8個 run全部 checker OK，每個有保留 schedule、stderr、模型、解檔（如有）及 solver log；資料存於 shared/tmp/codex/window-horizon/formal，重現與摘要見 notes/window-horizon-ablation.md。

C1355:2,2,1 固定原 heuristic UB=185、原有效 LB=178；四組均沒有改善 incumbent，最終保留合法185，wall time 51.29–51.60秒。gap四組均得到3並等於LB，正確證明最佳。

| 窗/目標 | H | binaries | rows | presolve | root及終止階段 |
|---|---:|---:|---:|---:|---|
| path / min L | 185 | 100219 | 256898 | 51.02s | 未觀測root；presolve耗盡預算 |
| resource / min L | 185 | 58982 | 143280 | 34.12s | barrier收斂至178，crossover逾時 |
| path / feasibility | 184 | 99576 | 255203 | 50.90s | 未觀測root；presolve耗盡預算 |
| resource / feasibility | 184 | 58339 | 141618 | 35.06s | barrier收斂至0，crossover逾時 |

強窗減少約41% binary，min-L LP由13,001,696降至7,231,071bytes。但僅將求解推進到較後階段，沒有帶來本次latency收益。UB−1比相同窗的min-L少643binary（每個operation少一格），不是大幅縮模。feasibility沒有可行MIP start，而min-L載入185；兩個feasibility run沒有整數解，不能把零目標LP的0當成排程解或最佳證明。

關鍵日誌證據：resource/min-L及resource/feas的 barrier均17 iterations，在 **solver累計時間42.13s/42.99s** 收斂；這不是額外42秒barrier耗時。隨後進入 Root crossover log，在約50秒仍剩903/1422個PPushes，並出現 `Crossover changed status from Optimal to Time Limit`。root摘要分別16.56/15.44秒，包含barrier、ordering、crossover等root階段；不可把未完成的root誤寫為barrier沒有收斂。兩個path組只有0 nodes，強窗組只有root的1 node，沒有看到分支樹搜尋成為主要成本。

結論範圍：在固定本路線UB185與共同Method2/Presolve2設定下，主要瓶頸先是presolve，再是crossover；時間窗收緊有效降低模型規模，但本次不降低latency。這不否定claude@55567d1的結果：對方有更好的heuristic與更緊horizon；不能將兩路不同UB的結果當成純solver設定差異。本輪未重做已知C1355:3,2,2=141的證明，也沒有搜尋新參數。

決定：production src保持不變。本階段未刷新default suite的best latency/LB，shared/best.tsv只追加自製gap的驗證來源，保留他路較強紀錄。已保存固定source manifest、完整phase摘要、48個驗證紀錄與8筆diagnostic結果到codex-results.tsv；harness不是eval.py，明確記載獨立check.py、60秒外部限制、55秒程式預算、CPU4–7與Threads4。

未完成且本輪不做：更好UB下的同因素比較、presolve/crossover設定消融、full suite與production整合。下一步若再授權，先整合或固定已驗證的較好heuristic UB，再決定是否值得針對presolve/crossover做單因素對照；避免重做已知141證明或把時間當成額外目標。到此按主持人要求停止。
