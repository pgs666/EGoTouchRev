# 手写笔坐标解算管线文档

> 最后更新: 2026-03-28

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                     原始帧数据 (Master + Slave)                      │
│                  rawData: ~5402 bytes total                         │
└──────┬──────────────────────────────────┬───────────────────────────┘
       │ Master Frame (5063 bytes)        │ Slave Frame (339 bytes)
       │ 触控mutual capacitance          │ 手写笔grid数据
       │ (本管线不使用)                    │
       ▼                                  ▼
┌──────────────┐                ┌──────────────────────────┐
│ Master Suffix │                │  Slave Header (7 bytes)  │
│ (128 words)   │                │  + Grid Payload (332)    │
│ 备用meta源     │                │  主要数据源               │
└──────────────┘                └──────────┬───────────────┘
                                           │
                    ┌──────────────────────┐│┌──────────────────┐
                    │ 1. ParseSlaveWords   │││ 2. ExtractMeta   │
                    │    校验+解析166 words ││  press/btn/status│
                    └──────────┬───────────┘│└──────────────────┘
                               │            │
                    ┌──────────▼───────────┐│
                    │ 3. ExtractGridFrom   ││
                    │    SlaveWords         ││
                    │    → TX1/TX2 9×9 grid ││
                    │    → anchor (row,col) ││
                    └──────────┬───────────┘│
                               │            │
                    ┌──────────▼───────────┐│
                    │ 4. FindPeak           ││
                    │    flood-fill peak    ││
                    │    detection on 9×9   ││
                    └──────────┬───────────┘│
                               │            │
                    ┌──────────▼───────────┐│
                    │ 5. ProjectTo1D       ││
                    │    行列求和投影       ││
                    └──────────┬───────────┘│
                               │            │
                    ┌──────────▼───────────┐│
                    │ 6. CoordinateSolver  ││
                    │    三角插值/重心法    ││
                    └──────────┬───────────┘│
                               │            │
                    ┌──────────▼───────────┐│
                    │ 7. Anchor偏移校正     ││
                    │    全传感器坐标重建   ││
                    └──────────┬───────────┘│
                               │            │
                    ┌──────────▼───────────┐│
                    │ 8. BuildStylusPacket ◄┘│
                    │    映射到HID报告      │
                    │    [0, 16000]         │
                    └──────────────────────┘
```

## 2. 数据帧结构

### 2.1 完整帧布局
```
Offset      Size    Description
──────────────────────────────────
0x0000      5063    Master Frame (触控数据, 手写笔管线不直接使用)
0x13C7      339     Slave Frame  (手写笔核心数据)
```

### 2.2 Slave Frame 结构

```
┌─────────── Slave Frame (339 bytes) ──────────────────────────┐
│                                                               │
│  ┌── Header (7 bytes) ──┐  ┌── Payload (332 bytes) ────────┐ │
│  │ [0..1] status (u16)  │  │ 166 words × 2 bytes = 332     │ │
│  │ [2..3] freq   (u16)  │  │                                │ │
│  │ [4..5] press  (u16)  │  │ ┌── TX1 Block (83 words) ──┐  │ │
│  │ [6]    button (u8)   │  │ │ word[0]: anchorRow        │  │ │
│  └──────────────────────┘  │ │ word[1]: anchorCol        │  │ │
│                             │ │ word[2..82]: 9×9 grid     │  │ │
│                             │ └───────────────────────────┘  │ │
│                             │ ┌── TX2 Block (83 words) ──┐  │ │
│                             │ │ word[83]: anchorRow       │  │ │
│                             │ │ word[84]: anchorCol       │  │ │
│                             │ │ word[85..165]: 9×9 grid   │  │ │
│                             │ └───────────────────────────┘  │ │
│                             └────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────┘
```

### 2.3 TX1/TX2 Grid Block

每个 Block 包含 83 个 uint16 words:
- **word[0]**: `anchorRow` — 9×9 窗口中心在完整传感器上的行号
- **word[1]**: `anchorCol` — 9×9 窗口中心在完整传感器上的列号
- **word[2..82]**: 9×9 grid 值 (int16)，按行优先排列

> **Anchor 含义**: anchor 代表 9×9 grid 的**中心位置** (index 4)
> 在完整传感器阵列上的坐标。这是固件在扫描时确定的笔的大致位置。

## 3. 坐标解算流程详解

### 3.1 Step 1: Slave Words 解析 (`ParseSlaveWords`)

```
输入: rawData[kMasterFrameBytes + 7 ..]
输出: uint16_t words[166]

处理:
  1. 跳过 slave header 的 7 字节
  2. 读取 166 个 little-endian uint16 words
  3. (可选) 校验和验证: sum(all_words) & 0xFFFF == 0
```

### 3.2 Step 2: Grid 提取 (`ExtractGridFromSlaveWords`)

```
输入: words[166]
输出: AsaGridData { tx1: FreqBlock, tx2: FreqBlock }

TX1 Block (words [0..82]):
  anchorRow = words[0]
  anchorCol = words[1]
  grid[r][c] = (int16_t)words[2 + r*9 + c]

TX2 Block (words [83..165]):
  anchorRow = words[83]
  anchorCol = words[84]
  grid[r][c] = (int16_t)words[85 + r*9 + c]

有效性判断:
  TX1.valid = (anchorRow != 0x00FF) || (anchorCol != 0x00FF)
  TX2.valid = TX1.valid  (TX2在无笔时输出垃圾)
```

### 3.3 Step 3: Peak 检测 (`GridPeakDetector::FindPeak`)

在 9×9 grid 上进行 flood-fill peak 检测:

```
算法:
  for each cell (r, c) in grid:
    if IsPeak(r, c):   // 4-邻域局部最大值 && > noiseThreshold
      FloodFill 计算连通区域大小
      if connectedPixels < maxConnected:  // 排除大面积噪声
        计算 3×3 邻域和
        保留邻域和最大的 peak

输出: GridPeakUnit { peakRow, peakCol, peakValue, valid }

参数:
  noiseThreshold = 50   (低于此值视为噪声)
  maxConnected   = 20   (超过此值视为噪声块)
```

### 3.4 Step 4: 1D 投影 (`GridPeakDetector::ProjectTo1D`)

将 9×9 grid 按行/列分别求和，生成两条 1D 信号:

```
projRadius = 2 (以 peak 为中心 ±2 行/列作为投影范围)

dim1[c] = Σ grid[rMin..rMax][c]   (c = 0..8)  → 列方向投影
dim2[r] = Σ grid[r][cMin..cMax]   (r = 0..8)  → 行方向投影

其中:
  rMin = max(0, peakRow - projRadius)
  rMax = min(8, peakRow + projRadius)
  cMin = max(0, peakCol - projRadius)
  cMax = min(8, peakCol + projRadius)

peakIdxDim1 = argmax(dim1)
peakIdxDim2 = argmax(dim2)
```

### 3.5 Step 5: 三角插值 (`CoordinateSolver`)

基于 1D 投影的 peak 位置进行亚像素精度插值。

#### 3.5.1 中间位置: `TriangleAlgUsing3Point`

这是逆向自 TSACore.dll 的原始算法:

```
输入: left (信号[peak-1]), peak (信号[peak]), right (信号[peak+1])

if right < left:  // peak 偏左
    minVal = min(right, peak-1)
    ratio = (left - minVal) * 1024 / (peak - minVal)
    result = -(ratio / 2)
else:             // peak 偏右或居中
    minVal = min(left, peak-1)
    ratio = (right - minVal) * 1024 / (peak - minVal)
    result = +(ratio / 2)

result += 0x200   // 加上半格偏移 (512)

最终坐标 = peakIdx * 0x400 + result
```

**结果范围**: `[peakIdx * 1024, (peakIdx+1) * 1024)` 内的亚像素位置

#### 3.5.2 边缘位置: `TriangleAlgEdge`

当 peak 在 grid 边缘 (index 0 或 8) 时，用虚拟邻居代替不存在的邻域:

```
EdgeCompensating(peak, n1, n2, param1):
  comp1 = (peak - n1) * 10 / param1
  comp2 = peak - (n1 - n2) * param1 / 10
  virtualNeighbor = max(comp1, comp2)
  if virtualNeighbor >= peak: virtualNeighbor = peak - 1

然后调用 TriangleAlgUsing3Point(virtualNeighbor, peak, n1)

左边缘(peakIdx=0): 直接使用结果
右边缘(peakIdx=8): result = 9*1024 - edgeResult
```

### 3.6 Step 6: Anchor 偏移 (全传感器坐标重建)

**这是最关键的步骤。**

9×9 grid 只是完整传感器阵列的一个局部窗口。
grid 内的坐标需要加上 anchor 偏移才能得到全传感器坐标:

```
gridLocalX = CoordinateSolver.dim1   (范围 [0, 9*1024) ≈ [0, 9216))
gridLocalY = CoordinateSolver.dim2

gridCenter = 4  (kGridDim / 2)
kCoorUnit  = 1024

fullSensorX = anchor.row * kCoorUnit + gridLocalX - gridCenter * kCoorUnit
fullSensorY = anchor.col * kCoorUnit + gridLocalY - gridCenter * kCoorUnit
```

**为什么要减去 gridCenter * kCoorUnit:**
anchor 是 grid 的**中心**: 当笔在 anchor 正上方时，
peak 位于 grid index 4，gridLocalX ≈ 4 * 1024 + 512 = 4608。
不减去中心偏移的话，坐标会固定偏移约 4096。

### 3.7 Step 7: HID 报告映射 (`BuildStylusPacket`)

将全传感器坐标映射到 HID 逻辑范围 [0, 16000]:

```
sensorRangeX = sensorDimX * 1024  (如: 37 * 1024 = 37888)
sensorRangeY = sensorDimY * 1024  (如: 23 * 1024 = 23552)

reportX = (1.0 - fullSensorX / sensorRangeX) * 16000   // X 轴反转
reportY = fullSensorY / sensorRangeY * 16000

输出: [0, 16000] → Windows 通过 HID 描述符映射到屏幕像素
```

> **注意**: X 轴反转 (`1.0 - ...`) 是横屏方向约定所需。

## 4. 原厂 vs 我们的实现差异

### 4.1 原厂 TSACore 的数据流

```
Himax 固件                ThpService (Himax驱动)           TSACore
  │                              │                           │
  ├── slave frame ──────────────►│                           │
  │   (9×9 grid + anchor)       │                           │
  │                              │ 创建 fullDim×fullDim      │
  │                              │ 零矩阵                   │
  │                              │                           │
  │                              │ 将 9×9 贴到              │
  │                              │ anchor 位置               │
  │                              │                           │
  │                              ├── 完整矩阵 ──────────────►│
  │                              │   (37×23 或类似)          │
  │                              │                           │
  │                              │                  完整维度 1D 投影
  │                              │                  Peak 在全局位置
  │                              │                  坐标直接正确
```

### 4.2 我们的等效方案

```
Himax 固件                EGoTouchService
  │                              │
  ├── slave frame ──────────────►│
  │   (9×9 grid + anchor)       │
  │                              │
  │                              │ 直接在 9×9 grid 上:
  │                              │   Peak detection
  │                              │   1D 投影 (长度=9)
  │                              │   三角插值
  │                              │   → gridLocal 坐标
  │                              │
  │                              │ 加上 anchor 偏移:
  │                              │   fullCoord = anchor*1024
  │                              │              + gridLocal
  │                              │              - 4*1024
  │                              │
  │                              │ 映射到 HID 范围
```

**等效性**: 两种方法在数学上等价。
原厂在完整矩阵中做的投影，只有 anchor 位置附近的 9×9 区域有非零值，
所以 peak 总是在 `[anchor, anchor+8]` 范围内，
最终坐标 = `(anchor + peakIdx) * 1024 + subOffset` — 与我们的结果一致。

## 5. 关键参数

| 参数 | 值 | 说明 |
|------|------|------|
| kGridDim | 9 | 网格维度 |
| kCoorUnit | 1024 (0x400) | 每传感器间距的子单位数 |
| kGridCenter | 4 | 网格中心索引 |
| sensorDimX | **待确定** (默认37) | 完整传感器行数 |
| sensorDimY | **待确定** (默认23) | 完整传感器列数 |
| noiseThreshold | 50 | Peak 检测噪声门限 |
| HID Report Max | 16000 | HID 逻辑坐标最大值 |
| 屏幕分辨率 | 2560 × 1600 | Gaokun CSOT 面板 |

### 5.1 确定 sensorDimX/Y 的方法

将笔移到屏幕四角，观察日志中的 anchor 值:

```
sensorDimX = maxAnchorRow + kGridDim    // 最大 anchorRow + 9
sensorDimY = maxAnchorCol + kGridDim    // 最大 anchorCol + 9
```

也可通过 GUI 中的 `Sensor Dim X/Y` 滑块实时调整，直到坐标范围正确覆盖屏幕。

## 6. Pressure 和 Button 提取

### 6.1 数据来源

压力和按钮数据从 **slave frame header** (前 7 字节) 提取:

```
slave[0..1]: status   (uint16 LE)
slave[2..3]: freq     (uint16 LE)
slave[4..5]: pressure (uint16 LE, 范围 [0, 0x0FFF])
slave[6]:    button   (uint8, bit 0 = 按下)
```

> **注意**: 具体字节布局可能需要根据实际日志调整。
> 日志标签 `SlaveHdr` 每 120 帧输出一次原始字节用于验证。

### 6.2 Pressure 处理 (`SolvePressure`)

```
1. 分段多项式映射:
   if raw <= 11:  mapped = min(raw, 1)
   elif raw <= 127: mapped = seg1 polynomial (4th order)
   else:           mapped = seg2 polynomial (4th order)

2. 增益: mapped *= gainPercent / 100

3. IIR 滤波: smoothed = prev * (128-w)/128 + mapped * w/128

4. 尾部衰减: 释放时逐帧递减

5. 输出范围: [0, 0x0FFF] = [0, 4095]
```

### 6.3 Button 处理 (`UpdateButtonState`)

```
if !active:  button = 0
elif rawBits & 1:  button = 1, hold counter = N
elif hold counter > 0:  button = 1, counter--
else: button = 0
```

Button 有 `releaseHoldFrames` (默认 2) 帧的维持时间防抖。

## 7. 文件索引

| 文件 | 职责 |
|------|------|
| `AsaTypes.h` | 常量、数据结构定义 |
| `GridPeakDetector.cpp` | Peak 检测 + 1D 投影 |
| `CoordinateSolver.cpp` | 三角插值坐标解算 |
| `CoorPostProcessor.cpp` | 坐标后处理 (IIR, jitter) |
| `StylusPipeline.cpp` | 主管线、帧解析、anchor偏移、HID报告 |
| `StylusPipeline.h` | 所有可配置参数定义 |

## 8. 已知问题与待办

- [ ] **sensorDimX/Y 待确定**: 需要观测实际 anchor 范围
- [ ] **Slave header 字节布局**: pressure/button 位置假设,需通过日志验证
- [ ] **Tilt 输出**: 暂时禁用,需正确 TX2 参数后启用
- [ ] **EdgeCompensating 参数**: 当前使用默认值, 应从 TSAPrmt 中提取
- [ ] **CoorMultiOrderFitCompensate**: 原厂有多项式校正, 当前未实现
- [ ] **SensorPitchSizeMap**: 原厂有非线性传感器间距映射, 当前用线性近似
