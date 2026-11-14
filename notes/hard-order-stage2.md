# Critical-block destroy/reinsert beam：負結果 pilot

在 stage1 的單點lane relocation零改善之後，本階段假設「多條critical paths需同時改動machine arcs」。沿用4f1f4c9完全相同seed，改用adaptive destroy/rebuild。沒有採用claude stage5/6的target DFS、時間窗傳播或plateau/FBI；已讀最新claude@172a217，其目前hard geo=1.00145，比最初公布1.00459更強。

## 方法與正確性

從聯合data+machine DAG隨機trace一條最長路，刪除其machine arc端點的lane連結；橋接前後鄰居，保留其他operation的lane相對順序與全部data edges。重複直到partial latency<incumbent，至少6個、上限依輪次6/10/14/24。未插回的operation保留data edges，但暫不占machine lane，所以partial schedule只作completion的有效LB，不作合法輸出。

Beam16逐一插回同kind lanes，候選位置為原始step±4/10內的anchors及每lane首尾。插入用a→u→b取代a→b，其傳遞含義仍在，partial longest-path LB不能下降。以(latency,pressure)排序；partial latency>best−1可剪枝。全部operation插回才接受並保留best-ever；正式入口另外validate。固定random seed，最多1.5B估計work、8秒額外wall deadline、96MiB狀態估計guard。

## 五個固定case（CPU4–7，全部check.py OK）

| case | seed→final | wall s | destroy / partial LB降低 | beam expansions / 完成 |
|---|---:|---:|---:|---:|
| layered_s129_n252:5,3,2 |31→31|11.89|1752 / 687|23855 / 0|
| funnel_s1210_n243:4,2,1 |72→72|12.33|2918 / 1986|32619 / 0|
| layered_s1008_n616:8,8,5 |35→35|16.81|32886 / 1|27 / 0|
| chains_s575_n2879:12,15,6 |113→113|12.75|479 / 4|567 / 0|
| layered_s1076_n851:8,8,6 |48→48|13.50|6912 / 27|1702 / 0|

五case同次seed→final為0改善/0退步，每case均耗盡1.5B work。這是針對性pilot，沒有重新跑完整50case，也不能把五case計成hard-suite geo。第一階段完整50case仍為固定分母1.00446541624，與claude同機baseline全平；本次沒有新證據超越它，更沒有超越最新claude1.00145。

## 驗證與診斷

- 120個隨機小DAG；5,710次relocation/undo、1,226個有環候選拒絕。
- 新增逐步detach/insert檢查：保留data precedence、每次插入partial LB不下降、重建原順序恢復完整lane state，最終resource合法。
- positive exact toy：8個獨立同kind operations、2資源，原排程刻意序列化latency8；beam確實完成重建並到optimum4（ceil(8/2)），避免把hard上的0完成誤判成程式永遠無法完成。
- 23個CLI輸出（random、15k deep/wide、tiny budget、oversized limits、h/e、disabled）全部check.py OK；單元與CLI在最終research dispatch修改後重跑通過。
- 最終只在`MLRCS_ORDER_METHOD=beam`時採用beam；預設保持原single，`MLRCS_ORDER_SEARCH=0`仍可停用全部order pass。沒有把零收益成本設成預設。

前兩case多數能打斷所有長路，但beam沒有完成改善重建，表示局部鄰域可能無L−1解，或beam寬度/排序過早丟掉必要前綴；目前沒有區分兩者。後三case主要連destroy都無法降低partial LB，d≤24不足以穩定解放全部critical paths。沒有採用「加大beam就一定有效」的結論。

## 重現與來源

`hard-order-stage2-pilot.py`編列五個case、CPU affinity、保存每個stdout/stderr/command/sidecar/checker log並寫獨立TSV；原始輸出在`shared/tmp/codex/hard-order-stage2-pilot/`。`evaluated-source/`保存逐檔hash核對的實際評估source與CMakeLists；`notes/hard-order-stage2-source-manifest.json`區分評估source及最後source。評估時beam是暫時預設；收尾改成明確research開關，beam內部沒有改動。腳本已加`MLRCS_ORDER_METHOD=beam`，可在最終source重現同演算法；wall-clock seed restart仍可能造成case結果差異。

單元：`taskset -c 4-7 c++ -std=c++17 -O1 -Isrc tests/resource_order.cpp src/bounds.cpp -o shared/tmp/codex/resource-order-lns-test`，再執行該binary。CLI：`taskset -c 4-7 python3 tests/check_resource_order.py shared/tmp/codex/build-lns-stage2/mlrcs`。Pilot：建置同路徑binary後執行`taskset -c 4-7 python3 notes/hard-order-stage2-pilot.py`。重跑會覆蓋同名pilot artifacts，請先另存舊資料。

五個正式pilot列已追加codex-results.tsv，tag=hard-order-stage2-pilot；每列獨立checker驗證，未冒充eval.py完整suite。shared/best.tsv已audit，全部結果高於現有best且未證明新LB，故不改其數字及更強來源。shared/questions.md通知負結果與research開關。

下一步回到原single搜尋，檢驗目前lexicographic greedy選擇是否阻止名義上的best+1退步；若嘗試真正annealing/first-admissible，須同次seed對照與固定work控制。這屬同一資源順序路線；本階段不再擴大beam評估。
