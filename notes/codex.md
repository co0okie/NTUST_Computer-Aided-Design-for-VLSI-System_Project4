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
