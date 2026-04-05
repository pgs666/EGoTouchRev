# EGoTouch 用户态驱动 → Windows ARM64 KMDF 内核驱动迁移说明

> 更新时间：2026-04-05  
> 目标：将当前 `EGoTouchService` 的“设备访问层”重构为 ARM64 内核驱动，并支持 INF 一键安装。

---

## 1. 本次重构产物

已新增内核驱动工程目录：

```text
KernelDriver/EGoTouchKm/
  Driver.cpp
  Driver.h
  Ioctl.h
  EGoTouchKm.inf
  EGoTouchKm.vcxproj
  EGoTouchKm.sln
```

已新增脚本：

```text
scripts/build_kmdf_driver.bat
scripts/install_kmdf_driver.bat
scripts/uninstall_kmdf_driver.bat
```

---

## 2. 架构映射（用户态 -> 内核态）

### 2.1 原用户态关键路径

- `ServiceHost -> DeviceRuntime -> Himax::Chip/HalDevice`
- 通过 `CreateFile + DeviceIoControl` 与底层 SPBTESTTOOL 设备交互
- 在用户态完成 AFE 命令、寄存器读写、GetFrame 等访问

### 2.2 新内核态接口

驱动导出统一控制设备：

- 设备名：`\Device\EGoTouchKm`
- 符号链接：`\DosDevices\EGoTouchKm`

IOCTL 协议定义于 `KernelDriver/EGoTouchKm/Ioctl.h`，包含：

- `IOCTL_EGO_PING`
- `IOCTL_EGO_GET_PROTOCOL_INFO`
- `IOCTL_EGO_SEND_AFE_COMMAND`
- `IOCTL_EGO_GET_LAST_STATUS`
- `IOCTL_EGO_READ_REGISTER`
- `IOCTL_EGO_WRITE_REGISTER`
- `IOCTL_EGO_GET_FRAME`

并已对齐 legacy SPI 控制码（从现有用户态 `HimaxProtocol.cpp` 提取）。

---

## 3. 代码实现状态

`Driver.cpp` 已实现：

- `DriverEntry`
- `EvtDeviceAdd`
- 默认 I/O Queue
- `EvtIoDeviceControl`
- IOCTL 分发与基础数据结构/缓冲区处理

当前为**可编译的内核驱动骨架**，已具备向“真实芯片访问”扩展的接口框架。  
其中寄存器/帧读取逻辑当前为 placeholder（用于联调与协议稳定），后续可将原 `HimaxProtocol` 的寄存器与 SPI 传输逻辑逐步内核化迁入。

---

## 4. ARM64 构建方式

前置条件：

1. 安装 Visual Studio 2022 + WDK（含 `WindowsKernelModeDriver10.0` 工具链）
2. 在开发者命令行中可执行 `msbuild`

构建命令：

```bat
scripts\build_kmdf_driver.bat
```

等价手动命令：

```bat
msbuild KernelDriver\EGoTouchKm\EGoTouchKm.sln /t:Build /p:Configuration=Release /p:Platform=ARM64
```

---

## 5. INF 一键安装

驱动 INF：`KernelDriver/EGoTouchKm/EGoTouchKm.inf`

安装脚本：

```bat
scripts\install_kmdf_driver.bat
```

脚本行为：

1. 检查管理员权限
2. 校验 INF/SYS/CAT
3. `pnputil /add-driver ... /install` 导入并安装
4. 扫描设备树触发 Root 枚举

卸载脚本：

```bat
scripts\uninstall_kmdf_driver.bat
```

---

## 6. 与现有服务的对接建议

当前服务里原有路径是直接访问 `\\.\Global\SPBTESTTOOL_MASTER/SLAVE`。  
迁移后建议改成：

1. 在 Service 中新增 `KernelBridge`（仅处理 `\\.\EGoTouchKm`）
2. 将 `HalDevice::Ioctl/ReadBus/WriteBus/GetFrame` 调用改为新 IOCTL
3. 保留 `DeviceRuntime/Engine/VhfReporter`，先做到“设备访问层替换”，算法层不动

---

## 7. 后续落地清单（建议）

1. 将 `HimaxProtocol::register_read/write` 迁入 KMDF（WDFIOTARGET 到下层总线）
2. 在 KMDF 中实现中断/超时/阻塞策略对应 `SET_TIMEOUT/SET_BLOCK`
3. 替换 placeholder `GET_FRAME` 为真实帧 DMA/PIO 路径
4. 增加 WPP/ETW 日志，便于内核调试
5. 完整签名链（测试签名/正式签名）+ Cat 生成流程固定化
