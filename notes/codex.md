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
