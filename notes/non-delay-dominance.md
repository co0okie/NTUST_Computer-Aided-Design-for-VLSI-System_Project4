# Unit-time non-delay dominance：理論審核與小圖驗證

假設同題目：每個operation長度恰為一個整數cycle、只使用自己種類的一單位資源、每種類capacity固定、precedence為finish-to-start、PI在0可用、目標為全部operation完成的makespan。沒有額外release/deadline、序列相依setup或多資源同時佔用等限制。

**命題：存在non-delay的最優排程。** 在所有makespan最優排程中選擇sum(operation start steps)最小者（有限個1..L時刻的排程，故存在）。若某step t有kind k空位，而且有該時刻ready但被排在s>t的operation u，就把u由s移到t。u的所有operation predecessors已於t之前完成；它的successors原本都在s之後，所以提前u不會破壞任一precedence。t的k資源有空位，s則釋放一單位；其他資源與operation不變。此移動不增加makespan且嚴格降低sum starts，矛盾。因此最小sum starts的最優排程必定每kind填滿min(capacity,ready count)，也就是non-delay。這是左移dominance，沒有提供快速找到最佳ready選擇的演算法。

**不能推出固定priority greedy最優。** toy有兩個獨立AND n0/n1，只有n1驅動OR n2，每kind capacity1。按id先選n0：step1 n0，step2 n1，step3 n2，latency3；先選n1：step1 n1，step2 n0/n2，latency2。兩者均non-delay。反例BLIF與兩份checker通過的輸出已保存在notes/non-delay-fixed-priority-gap*；它是固定id greedy的反例，**不是目前mlrcs -h/-e差異測資**。

獨立subset DP檢驗（非heuristic、非eval.py）：100個固定seed661704的DAG，3–10ops，各kind capacity獨立1/2。狀態為已完成mask；一種列舉各kind任意大小0..capacity的ready子集（排除全空自迴圈），另一種強制每kind選滿min(capacity,ready count)，兩者都窮舉子集內的operation選擇。最優latency **100/100相同**；未找到dominance反例。前者總4352states、後者841states。輸出100個BLIF與兩種最佳排程，**200/200 check.py OK**；固定priority toy兩份輸出也checker OK。總5.965秒，整個script CPU4–7。數據是理論交叉驗證，沒有hard geo或mlrcs runtime/latency新成績，亦沒有用有限隨機樣本代替上述證明。

重現：`taskset -c 4-7 python3 notes/validate_non_delay.py`；原始200份輸出在shared/tmp/codex/non-delay-validation，script為防覆寫會拒絕既有artifact目錄，重跑前另存該目錄或換DEST。明細notes/non-delay-validation.tsv，summary記script hash與分類；沒有第三方套件。codex-results.tsv只追加明確mode=theory-dp的摘要，非正式eval case。已讀questions，Q17通知；shared best無strict改善，保持較強紀錄。

結論：不需要為本題另開「explicit waiting可能必要」的heuristic方向。priority/choice仍可能很難，non-delay dominance不能解決這個組合選擇問題。到此結束本階段，沒有新增heuristic實作或啟動新實驗。
