# Stage4：CEM priorities＋serial earliest-insertion decoding

## 假設與理論修正

前三階段lane relocation、critical-block beam及Metropolis annealing均未低於hard seed。本階段在operation priority的分布上做cross-entropy style elite learning，避免只在固定lane邊附近取單點move；起點仍使用已註明來源的claude@f275c81 seed，沒有採用最新claude stage5/6 target DFS或plateau/FBI。

最初提出serial SGS時，以為先預約未來operation可以產生list scheduling以外的主動idle排程。實作時反證了這個說法：本題每個operation恰好一個整數slot；假如最後有空slot，且某個當時ready的operation被排在更後面，該slot在選到它時也空著，和earliest insertion矛盾。所以此decoder的最終排程仍non-delay，**不能宣稱擴大可表示解空間**。不同處是priority population的學習過程與eligible-selection trajectory。單元測例先預約未來的A(t2)，後選到的獨立A仍填回t1；故沒有捏造「產生主動idle」的正測試。另使用合法idle seed {2,3}，驗證壓縮成{1,2}。

## 方法與guard

`MLRCS_ORDER_METHOD=cem`才啟用，預設single保持。新檔src/priority_cem.cpp，以topological ready（所有前驅已分配位置）heap依key選下一個operation；其earliest start是前驅時間最大值＋1，從該位置起找最早有capacity的slot，per-kind counts與availability bitset維護資源。horizon=n，任何合法DAG至少有逐個operation序列解；沒有binary模型或solver。

seed按原step、id排序轉成priority。歸納可知新step不晚於原step：在放某operation時，所有原step更後的operation尚未佔位，故它的原slot仍足夠容納同step已放operation。保留合法best-ever，所有輸出仍經validate。

固定mt19937_64 seed；32 samples ×64 generations，加1次seed calibration，共最多2049 decodes。elite8，主要排序latency、同分取sink completion sum。依elite實際decoder selection rank更新diagonal mean/std（EMA=.3）；std floor=.10，另20%完全uniform exploration、20%原seed-centered noise，避免elite坍縮。只更新嚴格較短的輸出incumbent，learning distribution可由較差candidate引導；沒有承諾CEM全域收斂或最佳性。

n>5000不啟用；內存O(elite×n+n)，估計<4MiB（不含既存graph）；100M decoder work（op、edge和bitset掃描）上限，單candidate估算cost超過剩餘budget不再開始，且每64operations檢查wall deadline。最多額外8秒deadline，原程式總預算仍55秒。實際先到的wall guard可能使decode數不足固定上限，紀錄attempts以區分；不將wall-clock seed部分稱為deterministic。

## 驗證與重現

`tests/priority_cem.cpp`：120 random DAG的precedence/resource/latency、non-delay性與seed重pack每op不晚於原step；未來預約後填洞、合法idle seed修補、horizon不足、過期deadline、INT_MAX容量、n>5000guard；12獨立ops/2資源的toy8→6；兩次固定2049decode逐位元相同。全部通過。

既有resource-order white-box：5710 moves、1226有環拒絕，通過。以METHOD=cem執行tests/check_resource_order.py，23份CLI輸出全check.py OK，包含短預算、h/e、15k deep/wide及非法capacity拒絕。ASan仍因系統缺libasan未執行，不宣稱sanitizer結果。

編譯CEM單元：`taskset -c 4-7 c++ -std=c++17 -O1 -Isrc tests/priority_cem.cpp -o shared/tmp/codex/priority-cem-test`，再執行該binary。既有resource_order白箱目前需另link src/priority_cem.cpp，測試檔開頭命令已更新。

Pilot使用共用formal harness，固定CPU4–7、fresh Release build、60秒case外部timeout、每case獨立check並保存stdout/stderr/run.json；摘要再check一次。`notes/hard-order-stage4-pilot-command.json`保存完整命令與env；source snapshot及hash為同名source-manifest，結果先寫同名-results.tsv。重跑必須使用新tag以防覆寫。五case沿用stage2/3選擇，並非隨機代表性樣本，不能由它推算全50geo。

## 五case pilot結果與決定

| case | 同次seed→final | wall seconds | generations / decodes | work |
|---|---:|---:|---:|---:|
| layered_s129_n252:5,3,2 |31→31|10.04|64 / 2049|1,522,407|
| funnel_s1210_n243:4,2,1 |72→72|10.08|64 / 2049|1,280,625|
| layered_s1008_n616:8,8,5 |35→35|10.14|64 / 2049|3,870,561|
| chains_s575_n2879:12,15,6 |113→113|10.34|64 / 2049|14,113,004|
| layered_s1076_n851:8,8,6 |48→48|10.14|64 / 2049|5,278,224|

全部5case原checker與保留輸出重驗皆OK。每case全部2049個decode可行，64 generations完整跑完，因此此負結果不是wall/work截斷。總26,064,821 work；accepted strict改善均0。總wall50.74s、fresh build14.7s。此五個故意選出的差1case上geo(seed)=geo(final)=1.02136624985，**不是全50指標**。本階段依事先停止準則沒有擴大全50；上一全50仍stage1的1.00446541624、與同機claude全平，不能宣稱本階段超越claude公開1.00459。

來源snapshot aggregate SHA-256 `71d266226774c793be274c7757a409dcd4af47a9fc538546a8ca31c85c15ddeb`，base20af80d＋此階段working-tree；manifest逐檔source與實際snapshot核對。共用eval因shared symlink將commit欄記成6e2c6ab，實際身份依note/source-manifest hash，而非該欄。正式5列已追加codex-results.tsv，保留獨立-results與所有stdout/stderr/check元資料，未覆寫前階段。

已讀shared/questions.md，無待答問題。shared/best.tsv audit沒有strict latency或LB改善，保持較強紀錄，Q15通知結果。source研究開關保留，預設single沒有改動。這否定此一次diagonal CEM分布、noise幅度、sink-sum tie-break與2048population評估的組合，不否定所有distribution learning。初始noise=.25及floor=.10作用於normalized rank，對大圖相當很大幅的順序擾動；若繼續同路線，一個可檢驗的小假設是差1case需要保留更多seed結構，用一次局部noise對照而不是無界參數grid。尚未進行該對照，不宣稱noise為失敗原因。
