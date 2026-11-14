# Stage5：單一 CEM local-noise 對照

Stage4 `4fab4ac` 的normalized priority rank sd=.25、floor=.10、seed noise=.4，可能破壞差1step的好seed太多相對順序。本階段只做事先指定的一組局部noise：`.025 / .01 / .04`，`MLRCS_ORDER_METHOD=cem-local`；原`cem`仍可重現原參數。32×64population、elite8、EMA、20%global exploration、其餘seed與decoder、100M work、8秒保險、n<=5000guard均不變；不是參數grid。source變動抽出noise_parameters與單一dispatch，沒有另改solver或priority策略。

必要單元 `tests/priority_cem_local.cpp` 明確核對兩組參數、原值保留、local toy8→6，以及兩次2049decode結果與工作量一致，通過。沒有重跑未改的120random/23CLI/solver測試；正式五case仍獨立check且保留輸出再check。

重現：`notes/hard-order-stage5-pilot-command.json`保存固定CPU4–7的完整harness命令/env，source snapshot與逐檔hash在source-manifest。新的tag防覆寫，獨立-results.tsv後再合併codex-results.tsv。五case與stage2–4相同，不能將其geo當成全50指標。所有seed→final比較均同一次執行，避免時間限制seed restart的軌跡差異。

| case | seed→final | wall s | generations / feasible decode | work |
|---|---:|---:|---:|---:|
| layered_s129_n252:5,3,2 |31→31|10.05|64 / 2049|1,522,407|
| funnel_s1210_n243:4,2,1 |72→72|10.04|64 / 2049|1,280,625|
| layered_s1008_n616:8,8,5 |35→35|10.14|64 / 2049|3,870,561|
| chains_s575_n2879:12,15,6 |113→113|10.50|64 / 2049|14,112,926|
| layered_s1076_n851:8,8,6 |48→48|10.15|64 / 2049|5,278,224|

5/5當次check.py與保存輸出重驗通過，0strict改善/0退步；每case完整2049decode，非wall/work截斷。總wall50.88s/max10.50s/build16.3s。此指定五case geo(seed)=geo(final)=1.02136624985；沒有全50新指標。相較stage4，縮小noise仍未改善，不能把stage4失敗直接歸因noise過大，也不能把此一次負結果推論成所有局部population無效。

source aggregate `fb505327c533840d3ae03471cf6958f898c27de022371fcce25b105ad9de9e67`（base4fab4ac＋本階段改動），current source/snapshot逐檔核對通過。原TSV commit欄6e2c6ab仍為shared symlink metadata，身份以source-manifest/hash為準。已追加5筆正式codex-results.tsv；shared/best.tsv strict latency/LB改善皆無，保留較強數值與來源。已讀questions並追加Q16通知。

依預定停止準則，**不擴大全50、不再提出noise變體**。兩組cem/cem-local保留研究開關，預設single保持；新h/e差異測資、完整76suite與報告仍未完成，沒有新增最佳證明。
