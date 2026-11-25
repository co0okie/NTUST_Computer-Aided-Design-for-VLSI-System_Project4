# 11/19 規定移植：代表子集驗收

本階段只移植輸出檔與 Gurobi C API 契約，不改 heuristic 搜尋或 ILP formulation。Baseline 為 codex@1cd27a5；after 為同 HEAD 上 merge main@236f4b9 並修正衝突後的未提交 source，逐檔 SHA-256 與 immutable snapshot 見 resume2-{before,after}-source-manifest.json。shared symlink 下執行 eval 會把 Git metadata 指向主 worktree；結果 TSV 的 commit 已校正為 1cd27a5 / 1cd27a5+dirty，實際來源以 manifest 為準。

每階段 10 case × h/e：aoi_t481:1,1,1；aoi_big2 四組預設資源；aoi_C1355:3,2,2；hard layered129:5,3,2、funnel1210:4,2,1、layered1076:8,8,6；testcase/claude_gap4:3,3,3。兩階段各自 clean build，使用各自版本的 tools/eval.py、CPU4–7、Threads4、程式預算55秒、外部60秒。唯一私有 instrumentation 是在 case command 前加 /usr/bin/time，把 maximum RSS 寫到 stderr；不改 scheduler。before 按舊契約 check stdout；after 把 BLIF 複製進執行目錄，按新契約只 check 真正的 stem_h/e.out。原始 command、stderr、stdout、after 正式輸出檔與 hash 全部保存 shared/tmp/codex/resume2/。

## 結果

40/40 原 checker OK；保存輸出另以現行 tools/check.py 重驗 40/40 OK，並核對輸出 SHA-256。沒有 TIMEOUT / NO_OUTPUT / ERROR，兩階段均無 e latency > h。所有 case 的 h/e latency 與證明狀態完全相同。

| case | h 前/後 | e 前/後 | e status 前/後 |
|---|---:|---:|---|
| t481:1,1,1 | 883 / 883 | 883 / 883 | bound / bound |
| big2:1,1,1 | 7518 / 7518 | 7518 / 7518 | too_large / too_large |
| big2:2,2,1 | 3763 / 3763 | 3763 / 3763 | too_large / too_large |
| big2:3,2,2 | 2518 / 2518 | 2518 / 2518 | too_large / too_large |
| big2:8,8,4 | 946 / 946 | 946 / 946 | too_large / too_large |
| C1355:3,2,2 | 141 / 141 | 141 / 141 | timelimit / timelimit |
| layered129:5,3,2 | 31 / 31 | 30 / 30 | optimal / optimal |
| funnel1210:4,2,1 | 72 / 72 | 71 / 71 | optimal / optimal |
| layered1076:8,8,6 | 48 / 48 | 48 / 48 | timelimit / timelimit |
| claude_gap4:3,3,3 | 25 / 25 | 25 / 25 | optimal / optimal |

-e 證明最佳數 4/10 → 4/10（1 bound + 3 optimal）。claude_gap4 在移植前已 h=e25，對本路線不是有效的差異測資；本輪沒有另開測資搜尋。共享 best 沒有嚴格 latency/LB 改善，已 audit，保留既有較強值與來源。

| 指標 | gurobi_cl / stdout | C API / file |
|---|---:|---:|
| clean build | 13.5s | 14.9s |
| 20 run wall time 合計 | 325.06s | 334.97s |
| 最大單 case wall time | 51.27s | 52.67s |
| GNU time maximum RSS 峰值（C1355 e） | 2,386,096 KiB | 2,280,112 KiB |
| layered129 e runtime / RSS | 48.24s / 124,292 KiB | 52.13s / 133,008 KiB |
| funnel1210 e runtime / RSS | 17.39s / 100,872 KiB | 18.16s / 102,960 KiB |

這是單次順序對照，不能推論 C API 普遍加速/減速。before RSS 來自 GNU time 對 CLI parent/child accounting 的最大常駐量，after 來自 linked C API process，並非各行程 RSS 相加。C1355 既有模型仍約 2.28→2.17 GiB，超過專案希望單行程約2GiB的目標；本輪沒有因合規移植改善這個既有限制，也沒有追加記憶體/參數研究。t481 在兩組均直接 bound，big2 均 too_large，因此這些 run 不能用來比較求解器模型建立效能。

## 重驗與資料

`taskset -c 4-7 python3 notes/resume2_compare.py` 重驗全部保存排程並重算 paired TSV / summary JSON，不重跑 scheduler。`resume2-before-command.json` 與 after 同名檔保存實際完整 command、路徑與環境；raw build/run logs 和獨立 TSV 已入 notes，40列並已追加 notes/codex-results.tsv。snapshot 的 scheduler source 與目前 src/CMake hash 完全一致（after），但歷史工具/腳本仍保留舊契約作重現用途，並非交件 production 路徑。驗收只涵蓋指定代表子集，不宣稱完整76case或hard50回歸通過。
