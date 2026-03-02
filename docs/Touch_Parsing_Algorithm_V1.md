# Touch Parsing Algorithm V1（Python验证版）

## 目标
在“基线去除 + 比例阈值保留”之后，先完成一次有效初过滤，再做分流解析。目标不是一套算法覆盖全部场景，而是：
- 降低噪点与误拆分
- 在双指并拢/三指/快移下保持可分裂能力
- 对单指大面积按压（fat finger）优先保守单点输出

## 输入前提（来自 config.ini）
- BaseThreshold = 150
- Dynamic Row Deadzone / Spatial Sharpen 已在前级生效
- IIR 已关闭（Enabled=0）

## V1流程
1. **Blob提取**
   - 使用 `BaseThreshold` 做 8 连通域提取。
   - 小于 `min_blob_area` 的连通域直接忽略（默认 3）。

2. **峰值提取（候选结构）**
   - 连通域内做局部极大值检测（8 邻域）。
   - 峰值门限：`max(min_peak_abs, peak_ratio * blob_max)`。
   - 峰值NMS：按最小峰距去重（默认 2.0）。

3. **Fat-Finger 门控（先判定，后分裂）**
   - 面积大于 `fat_area_min`（默认 18）且满足以下之一：
     - 峰数量 <= 1
     - 双峰距离较近且谷底不深（`dist <= fat_close_peak_dist` 且 `valley_ratio >= fat_valley_keep_ratio`）
     - 次峰显著弱于主峰（`p2/p1 < fat_peak_uniqueness`）
   - 命中后：**强制单触点输出**（禁止分裂）。

4. **粘连分裂判定**
   - 多峰情况下，若任一峰对满足谷深条件（`valley/min_peak <= valley_ratio_split`），判定可分裂。

5. **分裂执行（Marker引导）**
   - 取前 K 个峰作为 seed（默认 3）。
   - 对 blob 内像素做峰值引导分配（简化 watershed 近似）。
   - 每个子 blob 计算加权质心输出触点。

6. **普通单点路径**
   - 不命中 fat finger 且不满足分裂条件时，直接对 blob 做加权质心。

## 当前验证脚本
- 脚本：`build/touch_algo_v1_validate.py`
- 数据：`build/exports/dvr/*.csv`（当前 5 个回放）

## 本轮运行结果（摘要）
- 旧版（V1）在 3 指场景召回偏低，平均触点明显低于目标。

## V1.1（无时域）改动
1. 增加“空域方差驱动分裂”判据：
   - 3个峰直接允许分裂；
   - 峰谷判据不成立时，使用 `blob_intensity_variance + blob_aspect_ratio` 兜底。
2. 增加“残差剥离分裂（Residual Peak Peeling）”：
   - 对重叠峰进行迭代减核，释放肩峰，提升 3 指分离灵敏度。
3. 增强 Fat-Finger 防过分裂：
   - 大面积且高细长形态（`area>=24 && aspect>=8`）直接判单指。

## V1.1 方差评估（当前 5 回放）
使用指标：
- `mean_contacts`：平均触点数（接近目标更好）
- `var_contacts`：触点数量方差（越低越稳）
- `mse_to_target`：触点数到目标指数量的均方误差（越低越好）

结果：
- `3Finger.csv`: target=3, mean=2.392, var=0.730, mse=1.100
- `dvr_backtrack_20260302_162257.csv`: target=2, mean=2.260, var=0.797, mse=0.865
- `dvr_backtrack_20260302_162347.csv`: target=2, mean=1.785, var=0.494, mse=0.540
- `dvr_backtrack_20260302_162417.csv`: target=2, mean=1.825, var=0.561, mse=0.592
- `singleHugeFinger.csv`: target=1, mean=1.000, var=0.000, mse=0.000

结论：
- 在不引入时域分裂的前提下，3 指召回显著提升（从约 1.43 提升到约 2.39）。
- 单指重压误拆已被压制到 0（当前回放）。
- 两个双指回放略有过分裂/波动，下一步需要做“空间约束型抑制”而非时域平滑。

## 审核关注点
1. singleHugeFinger 的误拆分是否可接受（目前已明显下降，但仍有少量 split）。
2. 双指快移回放里 `merged_split` 占比是否符合预期（是否需要更激进或更保守）。
3. 是否进入 V1.1：
   - 增加“时序稳定门”（短时一致性后再确认 split）
   - 增加“分裂后最小子 blob 面积占比”约束
