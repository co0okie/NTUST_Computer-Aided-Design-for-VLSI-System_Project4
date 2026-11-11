# 研究目標

實作 `mlrcs`：在資源限制下最小化 latency 的排程（ML-RCS）。題目只有 AND、OR、NOT 三種 operation，每個都是 1 cycle。

- `-h`：heuristic（例如 list scheduling），要快，latency 要盡量接近最佳。
- `-e`：以 `-h` 的結果為基礎，寫成 ILP，用 Gurobi 求出 exact 解。要在時限內證明最佳的 case 越多越好；證明不了時，也要輸出目前找到最好的合法排程。

題目見 [docs/Project04/Project04.md](docs/Project04/Project04.md)（[PDF](docs/Project04.pdf)），BLIF 格式見 [docs/BLIF/BLIF.md](docs/BLIF/BLIF.md)。

除了程式，還要交：
- 一個自己設計的測資，讓 `-h` 與 `-e` 的 latency 不同。
- 最多 3 頁的報告，內容包含：
  - heuristic 演算法與 ILP 模型，說明 precedence 與 resource 限制怎麼表達、heuristic 結果怎麼用在 ILP 裡；
  - 開發過程中的設計選擇與改進，要有實驗佐證；
  - 兩種方法在相同輸入、相同限制下的 latency 與 runtime 比較；
  - 分析上述差異測資：差異的成因，以及 heuristic 的優缺點。
- 題目鼓勵 GUI、輔助分析工具或其他特色功能。

研究過程中就要留下報告需要的素材（數據、圖、反例），不要等到最後才補。

# 硬性規則

規格以 docs 為準，這裡補充容易出錯、或 docs 沒寫清楚的地方。

- 輸出格式要與題目範例逐字一致：
  - 標題：`-h` 是 `Heuristic Scheduling Result`，`-e` 是 `ILP-based Scheduling Result`；
  - 每一步一行 `t: {AND} {OR} {NOT}`，空集合寫 `{}`；
  - 最後兩行是 `LATENCY: n` 與 `END`。
  - 同一個大括號內的順序，題目沒有規定。骨架依 BLIF 中的定義順序輸出，與範例一致。
- PI 不是 operation，step 0 就可以使用；operation 的 step 從 1 開始。
- 只能透過 `gurobi_cl` 命令列使用 Gurobi：寫出 LP 檔，再讀回解檔。不要 link Gurobi 的 C/C++ library，因為評測環境不一定有 header，路徑也可能不同。
- `-e` 必須先確認 `gurobi_cl` 存在而且能執行，否則印出錯誤訊息並以非 0 結束（骨架已經實作）。
- `-e` 的結果絕不能比 `-h` 差。最後一定要輸出一個合法排程：Gurobi 逾時、被中斷，或模型太大而沒有求解時，都要輸出目前最好的解。
- 建置設定（沿用前幾次作業的規定，Project 4 的 PDF 沒有另外寫明）：
  - C++17、CMake。
  - server 不會指定 `CMAKE_BUILD_TYPE`，所以 `CMakeLists.txt` 要自己預設 Release。
  - 不要用 `-march=native`。
  - 評測環境沒有網路，也沒有預裝的第三方 library。
  - build 限時 60 秒（目前 clean build 約 12 秒）。
- 時間限制：每個 case 60 秒，包含 I/O 與 Gurobi。程式用 wall clock 自行控制，預設總預算 `--time 55`。
  - Gurobi 只會偶爾檢查 TimeLimit：在 presolve 與 root LP 時，可能超時 10 秒以上。所以骨架到時間會先送 SIGINT，再送 SIGKILL。
  - 評測機器可能比我們慢，時間要留餘裕。
- 多執行緒：評測機器的核心數未知，Gurobi 的 `Threads` 預設由 Gurobi 自行決定。自己寫的平行化必須確認在 1 核下不會變差。
- 交件：學號命名的 `.tgz`，內含 C/C++ 原始碼與一個純文字 ReadMe，說明如何編譯、執行。
- 可以參考論文、講義與網路上的演算法，但不得直接複製他人的程式碼，包括同學、往年作品與開源 scheduler。

# 評分方式（推測）

Project 4 的評分方式還沒公布，以下參考 Project 2 的評分標準（`~/data/school/ntust/115-1 .../Grading Criteria_*.pdf`）：
- 用隱藏測資評分，「能在時限內跑完、輸出通過 verifier」是主要分數；
- 報告約佔 10 分；
- 老師的立場是「你應該自己掌握演算法的複雜度」。

因此優先順序是：
1. **穩健**：任何合法輸入、任何資源限制，都要在時限內輸出合法排程，不能 crash，也不能逾時。
2. **正確**：`-e` 在能證明最佳的情況下，輸出的確實是最佳解。
3. **品質**：`-h` 的 latency 越低越好；`-e` 在時限內證明最佳的 case 越多越好；兩者的 runtime 越短越好。

隱藏測資的規模可能比 `benchmark/` 還大，要考慮最壞情況的複雜度與記憶體。

## 研究指標

指標以 `tools/eval.py` 的預設 suite 為準：19 個電路 × 4 組資源限制（`--list`），`big4` 與 `big3` 內容完全相同，所以不列入。

- **best known**：每個 case 目前已知的最佳 latency 與最強的 lower bound，記錄在 `shared/best.tsv`（見下方），三條路線共同維護。
- `-h`：所有 case 的 `latency / best_known` 取幾何平均，越接近 1 越好；另外記錄總 runtime。
- `-e`：證明最佳（`status` 為 `optimal` 或 `bound`）的 case 數、未證明 case 的 `latency / best_known`，以及總 runtime。

# 目錄與工具

```
Project4/                 main（只放共用環境；研究在各自的 worktree）
├── AGENTS.md, CLAUDE.md  研究規範（CLAUDE.md 只是引用 AGENTS.md）
├── README.md             交件用的 ReadMe（各路線可修改）
├── docs/                 題目與 BLIF 格式（PDF 與 markdown）
├── benchmark/            課程提供的測資；自製測資也放這裡
├── CMakeLists.txt, src/  mlrcs 共用骨架（見下方），在各自的 branch 上發展
├── tools/                共用工具（見下方）
├── shared/   (ignored)   三條路線共用：questions.md、best.tsv、papers/、tmp/
├── claude/   (ignored)   git worktree，branch `claude`（Fable 5.2 主導）
├── codex/    (ignored)   git worktree，branch `codex`（GPT-6 Astra 主導）
└── human/    (ignored)   git worktree，branch `human`（使用者主導）
```

## 共用骨架（`CMakeLists.txt`、`src/`）

骨架可以直接編譯、執行，輸出也能通過 checker。各自的 branch 可以任意修改或重寫。

- `CMakeLists.txt`：預設 Release，輸出 `build/mlrcs`，自動收錄 `src/**/*.cpp`；`-DMLRCS_SANITIZERS=ON` 可以開 ASan / UBSan。
- `main.cpp`：
  - CLI 為 `mlrcs -h|-e BLIF AND OR NOT [--time 55] [--threads N] [--keep DIR] [--stats]`；
  - `--stats` 會在 stderr 印出一行 `mlrcs-stats key=value ...`，供 `eval.py` 讀取。
- `result.hpp`：`Result<T>`、`Error`，以及 `TRY` / `TRY_VOID` 巨集。
- `netlist.hpp`：以 SoA 存放的 `Netlist`。
  - node id 的順序：先是 PI（照 `.inputs` 順序），再來是 operation（照 `.names` 在檔案中的順序）。
  - fanin / fanout 以 CSR 存放，並附 operation 的拓撲順序 `topo`。
- `blif.hpp`：`read_blif`。
  - 支援 `\` 續行與 `#` 註解，忽略 `.default_*` 等不影響邏輯的指令。
  - 每個 `.names` 都必須是純 AND / OR / NOT，否則回傳錯誤。
  - 也會檢查：重複定義、未定義的訊號、組合迴圈。
- `schedule.hpp`：
  - `Resources`、`Schedule`；
  - `validate`：獨立檢查 precedence、resource 與 latency。每個輸出前都要通過，骨架已經這樣做。
  - `print_schedule`；
  - `asap`、`alap`；
  - `lower_bound`：critical path 與 `ceil(ops_k / limit_k)` 取大者，很弱，有很大的改進空間。
- `heuristic.cpp`：`list_schedule`，最基本的 list scheduling。priority 是 height（到 sink 的最長路徑），同分時取 id 較小者。
- `gurobi.hpp`：
  - `find_gurobi_cl`：在 PATH 與 `$GUROBI_HOME/bin` 中尋找，並實際執行 `--version` 確認能跑。
  - `ScratchDir`：暫存目錄，結束時刪除；給 `--keep` 時保留。
  - `run_gurobi`：以子行程執行，時間到先 SIGINT、再 SIGKILL；讀回 `.sol` 解檔。
- `ilp.cpp`：`exact_schedule`，最基本的 time-indexed ILP。
  - 每個 operation 的 binary 變數只開在 `[ASAP, ALAP(L_h)]` 時間窗內，`L_h` 是 heuristic 的 latency；
  - heuristic 的解當作 MIP start；
  - `L_h` 等於 lower bound 時直接跳過 ILP（status `bound`）；
  - 變數超過 300 萬個時不求解（status `too_large`）。
  - 目前在中大型 case 上，Gurobi 光 presolve 與 root LP 就會用完時間（例如 `t481:1,1,1` 有 84 萬個變數，LP 檔 113 MB），也幾乎沒有改進過 heuristic 的解。這是 `-e` 主要的研究題目。

## 工具

- `python3 tools/eval.py [--src DIR] [--mode h,e] [cases...] [--tsv FILE] [--note ...]`：照評測流程評估。
  - 先把 `CMakeLists.txt` 與 `src/` 複製到 `shared/tmp/` 下的全新目錄，從頭 build 並計時。
  - 接著逐一執行每個 case（每次 60 秒逾時），再用 `check.py` 驗證；每個 case 跑完就把一列寫進 `--tsv`。
  - case 寫成 `BENCH:AND,OR,NOT`，例如 `aoi_C1355:2,2,1`；BENCH 也可以是 `.blif` 檔的路徑。`--list` 列出預設 suite。
  - 用 `taskset` 綁定 CPU：branch `claude` 用 CPU 0–3、`codex` 用 4–7、其他用 8–11；Gurobi 預設 `--threads 4`。三條路線同時跑分也不會互相干擾。
  - 跑完整個 suite 的 `-e` 最多約 76 × 60 秒，大約要一個多小時。平常先用代表性的子集，正式記錄時才跑全部。
- `python3 tools/check.py BLIF AND OR NOT OUTPUT [--mode h|e]`：獨立的 checker。
  - 自行重新解析 BLIF，檢查格式、每個 operation 恰好出現一次、種類正確、precedence、resource，以及 LATENCY。
  - 指標一律以 checker 的結果為準，不要只相信 `mlrcs` 自己印出的數字。
- Gurobi 安裝在 `/opt/gurobi`（13.0.3，academic license，沒有規模限制）。非互動式 shell 不會讀 `~/.bashrc`，要手動 `export PATH=/opt/gurobi/bin:$PATH LD_LIBRARY_PATH=/opt/gurobi/lib`；`eval.py` 會自動補上。
- 之後的 Python 工具若需要第三方套件，一律用 PEP 723 宣告，以 `uv run` 執行。
- 共用工具有 bug 或需要新功能時，告訴使用者，或直接修改後在 `shared/questions.md` 說明，不要在自己的 branch 偷偷分歧。

# 三路協作

三條路線同時研究同一個目標。目的不是加快速度，而是**互相分享研究成果**：一條路線的發現、反例、失敗經驗，都可以讓另外兩條少走冤枉路。

## Git

- 三個 worktree 共用同一個 git 物件庫。對方一 commit，就能直接 `git log codex`、`git diff claude codex`、`git show human:src/...`，不需要 fetch。
- 可以自由 rebase 自己的 branch，也可以使用 `claude/*` 等額外 branch。不要改動別人的 branch。
- 可以隨時查看並自由採用別人的方法；採用時在筆記中註明出處（branch 與 commit）。

## 筆記（各自的 branch）

- `notes/<路線>.md`：研究日誌（動機 → 推導 → 實作 → 結果 → 結論），以及尚未嘗試的想法清單。
- `notes/<路線>-results.tsv`：`tools/eval.py --tsv` 的輸出，每次正式評估都要記錄，包括失敗的。

## 共用區（`shared/`）

- `shared/questions.md`：路線之間的問答與通知，格式見檔案開頭。
  - 想知道別人為什麼這樣做、想請別人驗證某個想法、發現共用工具的 bug、找到值得一看的結果，都寫在這裡。
  - 每個研究循環開始時，以及每次正式評估之後，都要看一次有沒有給自己的問題或通知，有就回答。
  - 回答時要具體，附上 commit、數據或檔案路徑。
- `shared/best.tsv`：每個 case 的 best known latency 與最強的 lower bound。
  - 欄位：`case	best_latency	lower_bound	source	note`；
  - 刷新紀錄時，必須是 `check.py` 驗證過的輸出，並附上來源（branch、commit、參數）。
  - `best_latency == lower_bound` 代表已經證明最佳。
- `shared/papers/<短名>/`：論文 PDF 與 `marker_single` 轉出的 markdown。轉檔前先看是否已經有人轉過，轉完在 `questions.md` 通知一聲。

## 自製測資

讓 `-h` 與 `-e` 結果不同的測資，放在 `benchmark/` 並 commit 到自己的 branch。要附上生成方式（腳本或說明），讓別人也能用。

# 省 token 的工作方式

主導者（Fable 5.2、GPT-6 Astra）的 context 與 token 都很寶貴，應該用來思考、決策與統整。

- 實作細節、大量讀檔、跑實驗、整理數據，盡量交給便宜的 subagent，並給清楚的規格與驗收條件。例如：
  - 「照這個 LP 模型改寫 `ilp.cpp`，`eval.py` 指定的 case 要全部 OK」；
  - 「跑這組 case，回報表格與異常」。
- subagent 只回報結論與關鍵數字，不要把大段 log 帶回主 context。
- 長時間的評估在背景執行，等待時繼續思考下一步。

# 研究方式

模仿學術界研究流程，包含：

1. 動機
    - 來自某個直覺，「我覺得這裡應該...」
    - 來自動機實驗，觀察到某種現象所做的假設，例如「從某些 case 可以觀察到...規律，或許...」
    - 在前幾輪研究發現的新東西，建議有想法就記下來，以免忘記
    - 沒有想法的話，可以參考別的路線的方法、在 `questions.md` 提問，或開 subagent 問他有什麼想法
    - 或者 paper survey：
        - 可以用 `~/.codex/skills/ieee-xplore-firefox` skill（需要 140.118.0.0/16 的 IP），或在其他網站尋找、下載論文；
        - 用 `marker_single` 轉成容易閱讀的 markdown，放在 `shared/papers/`；
        - `marker_single` 很花時間，盡可能不要重複轉同一份 paper。
2. 理論推導：試著用文字、數學思考該方法的合理性，能找到 theorem 最好，heuristic 也沒關係。
    - 如果光靠推導就能證明該方法沒有比較好，就回到 1. 動機，重新找方法。
    - 沒辦法證明也沒關係，由實驗結果證明一切。
    - ILP 的強度（LP relaxation 有多緊）與模型大小，都可以先用小 case 實驗。
3. 實作，並以 `tools/check.py` 驗證正確性。
4. 實驗：以 `tools/eval.py` 評估 latency 與 runtime，證明方法有效；有刷新紀錄就更新 `shared/best.tsv`。
5. 回到 1. 動機

如此循環往復，持續進步。想法可以大膽一點，只要符合規定並能達成最終目標，不擇手段。

# 機器資源

這台筆電（i9-12900H）有 6 個 P-core（CPU 0–11，含 hyperthread）與 8 個 E-core（CPU 12–19），只有 15 GB RAM，三條路線與使用者的桌面程式共用。Project 3 曾經兩次因為 OOM，連同 agent 的 session 一起被 kernel 砍掉。

- Gurobi 很吃記憶體：`t481:1,1,1` 的 84 萬個變數就用了約 1.5 GB。同時跑多個 Gurobi 之前，要先估算總記憶體用量。
- `/tmp` 是 tmpfs，內容佔用的是 RAM。大量的實驗輸出（包括 `--keep` 的 LP 檔）不要放在 `/tmp`，請放在 `shared/tmp/<路線>/`（在硬碟上，已 ignore），用完就刪。
- 單一行程（包括分析用的 Python 腳本）盡量不要超過 2 GB。
- 不要把大量輸出 pipe 進 Python 或 shell 變數；要寫成檔案再串流處理。
- 計時的實驗只能在自己分配到的 CPU 上跑，不要佔用別人的核心。

# 程式規則

- 使用 C++17，以 CMake 建置。
- 偏好 structure of arrays (SoA)。
- 偏好 Rust style：
    - 用自製的 `Result` 處理錯誤，不使用 throw / try / catch；
    - 不使用複雜的 inheritance 與 virtual，改用純資料 struct 加上自由函式（或純 method 的 trait 式 struct）；
    - const by default；
    - 用 RAII 管理資源，不要手動 `new` / `delete`、不要有擁有權不明的 raw pointer；
    - 不使用可變的全域狀態；
    - 巨集只用於 `TRY`；
    - 避免隱式轉換與過度的 template 技巧。
- 每個輸出的排程，都必須先通過 `validate`。

# 可參考資料

- `~/data/school/ntust/115-1 Computer-Aided Design for VLSI System 電腦輔助晶片系統設計/`：本課程的資料夾，其中 `L11_scheduling.pdf` 與本題直接相關，`L12_binding.pdf` 可能也有幫助。
- 先前專案的協作架構與工具：`~/repo/github/co0okie/NTUST_Computer-Aided-Design-for-VLSI-System_Project3`。
