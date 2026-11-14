# Stage3：single-order simulated annealing，負結果

## 動機與方法

Stage1名義上容許best+1，但每輪先選全鄰域的最小(latency,pressure)，只要有等長候選就不肯真正退步。本階段直接逐個隨機抽樣move，再做Metropolis acceptance；沿用原seed，沒有整合claude stage5/6新方法。

`MLRCS_ORDER_METHOD=anneal`才啟用；預設仍single。每次75%選slack≤1 operation、25%選任意operation；一半試鄰接交換，一半試同kind近距離relocation。聯合DAG有環或latency>best+1即拒絕，其餘用E=latency+0.2×pressure/(4096n)；pressure項最多0.2cycle，故+1cycle必為正energy。ΔE≤0必接受，否則機率exp(−ΔE/T)。固定seed，每個work epoch以幾何方式T=.4→.02cycle冷卻，開始時+1cycle約8.2%接受；350M work分8個epochs，epoch間從best重pack lanes，不做參數grid。工作量額度與原single同350M，但候選取樣和實際每候選成本不同，因此不是只改acceptance的單因素比較。額外8秒wall cap、best-ever合法輸出不變。

## 五case pilot（全部check.py OK）

| case | seed→final | wall s | accepted uphill / plateau / downhill |
|---|---:|---:|---:|
| layered_s129_n252:5,3,2 |31→31|11.05|8 / 65120 / 0|
| funnel_s1210_n243:4,2,1 |72→72|11.29|9 / 162407 / 1|
| layered_s1008_n616:8,8,5 |35→35|11.23|8 / 14343 / 0|
| chains_s575_n2879:12,15,6 |113→113|11.33|9 / 1951 / 1|
| layered_s1076_n851:8,8,6 |48→48|11.28|8 / 15394 / 0|

每case350M work、7次restart；42次真正latency uphill、259215次等長、2次下降，沒有一次低於seed。多數epoch接受一次退步後在best+1 plateau游走到restart。這否定「只要讓這個單點鄰域真正退步就會改善」的本次設定，不能推論所有annealing或資源順序方法都無效。

沒有完整50case評估，也沒有新的hard-suite geo。上一完整結果仍固定分母1.00446541624、與同機claude@f275c81全部相同；不能宣稱這個新增方法勝過公開1.00459，更尚未達到最新claude1.00145。best/LB無strict改善，shared/best.tsv audit後維持現有紀錄。

## 驗證、來源與重現

接受函式明確測試ΔE=1、T=.4時uniform=.05接受、.10拒絕，及非正ΔE必接受。12個獨立同kind ops/2資源、初始latency8的toy到optimum6。既有120小圖（5710 move、1226 cycle拒絕）通過；23份CLI輸出在anneal mode通過check.py，含tiny budget、h/e與15k deep/wide。沒有重跑ASan或未改的solver測試。

原始stdout/stderr、指令、每casechecker log、seed/final與SA counters在`shared/tmp/codex/hard-order-stage3-pilot/`；`evaluated-source/`逐檔與實際來源hash核對。manifest是notes/hard-order-stage3-source-manifest.json，base0c878af+本階段working tree；5筆正式pilot已追加codex-results.tsv，非eval.py完整suite。

重現：CPU4–7建置到`shared/tmp/codex/build-lns-stage2/mlrcs`，執行`taskset -c 4-7 python3 notes/hard-order-stage3-pilot.py`。腳本固定五case、METHOD=anneal、保留輸出並check；同名舊artifact會被覆蓋，重跑前另存。單元編譯與stage2相同，執行新tests/resource_order.cpp；CLI可用`MLRCS_ORDER_METHOD=anneal taskset -c 4-7 python3 tests/check_resource_order.py shared/tmp/codex/build-lns-stage2/mlrcs`。seed部分依wall-clock restart，重跑可能有seed差異，必須同次比較seed→final。

本階段停止擴大SA。下一步若繼續同路線，應先診斷需要改動的critical machine-edge cut大小或新解與seed的order差距，再決定更結構化的鄰域；不再盲目增加溫度/beam參數。
