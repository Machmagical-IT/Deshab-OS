# USB驱动调试文档

## 问题描述
在QEMU UEFI模式下，USB设备枚举失败。尝试使用UHCI和EHCI控制器都无法成功枚举USB设备。

## 最新调试进展 (2026-04-29 18:00) - 最终结果

### 问题根源

**QEMU PIIX4 UHCI控制器在控制传输的多阶段处理上存在限制**。

具体表现：
1. ✅ Setup TD成功处理（status从0x18800000变为0x18000007）
2. ✅ QH element被QEMU更新为指向Data TD  
3. ❌ Data TD的ACTIVE位从未被清除（QEMU不处理Data TD）

### 已尝试的修复方法（全部失败）

1. TD链一次性提交 - 使用Depth First位链接TD链 ❌
2. 分阶段提交 - 每个阶段独立提交和等待 ❌
3. TD链接格式修复 - 修正Depth First位和物理地址编码 ❌
4. Token匹配验证 - 确认Setup和Data TD的token匹配 ❌
5. 增加等待时间 - 从1秒增加到3秒 ❌
6. QH element重新设置 - 尝试触发QEMU重新处理 ❌

### 可能的解决方案

1. 使用QEMU ICH9 UHCI控制器：`-device ich9-usb-uhci,id=uhci`
2. 使用EHCI USB 2.0控制器：`-device ich9-usb-ehci,id=ehci`
3. 使用XHCI USB 3.0控制器：`-device qemu-xhci`
4. 使用PS/2鼠标作为备选方案

### 调试输出示例
```
[UHCI_CTRL] TD chain: setup->link=0x0389702C data->link=0x038970C4
[UHCI_CTRL] Waiting... Setup=0x18000007 Data=0x38800000 Status=0x38800000
[UHCI_CTRL] Transfer TIMEOUT
```

### 之前修改：分阶段提交控制传输（已回退）

**问题根源**：一次性提交Setup+Data+Status TD链，QEMU UHCI不自动处理后续TD。

**新方案**：参考Linux UHCI驱动，采用分阶段提交方式：
1. **阶段1**: 提交Setup TD，等待完成
2. **阶段2**: 提交Data TD（如果有），等待完成，复制数据
3. **阶段3**: 提交Status TD，等待完成

**关键修改**：
- 每个阶段独立设置`ctrl_qh->element`指向当前TD
- 每个阶段独立轮询TD的ACTIVE位
- 阶段之间添加延迟（2ms）确保QEMU有时间处理
- 不再依赖TD链的自动链接

**调试输出**：
```
[UHCI_CTRL] Phase 1: Submitting Setup TD
[UHCI_CTRL] Setup TD done, status=0x???
[UHCI_CTRL] Phase 2: Submitting Data TD
[UHCI_CTRL] Data TD done, status=0x???
[UHCI_CTRL] Phase 3: Submitting Status TD
[UHCI_CTRL] Status TD done, status=0x???
```

**预期结果**：
- Setup TD: ACTIVE清除，actual_length=7（8字节setup包）
- Data TD: ACTIVE清除，包含设备描述符数据
- Status TD: ACTIVE清除，零长度包确认

**待测试**：需要在QEMU中运行并观察调试输出

### 之前修复的问题（已保留）

1. **TD max_len字段溢出** (CRITICAL) - 使用uint32_t避免移位溢出
   ```c
   // 错误: uint16_t max_len = ((length - 1) & 0x7FF) << 21;  // 溢出为0
   // 正确: uint32_t max_len = ((uint32_t)((length - 1) & 0x7FF)) << 21;
   ```

2. **TD链接指针** - 不使用Depth First位，使用简单的物理地址链接
   ```c
   // 错误: setup_td->link = data_td_phys | UHCI_TD_LINK_DEPTH_FIRST;
   // 正确: setup_td->link = data_td_phys;  // 无标志位
   ```

3. **零长度包编码** - Status TD使用0x7FF表示零长度包
   ```c
   // 错误: ((0) << 21)  // 表示1字节
   // 正确: ((0x7FF) << 21)  // 表示零长度包
   ```

### 之前修复的问题（已保留）

#### 1. TD max_len字段溢出问题 (CRITICAL)
**问题**: Data TD的max_len使用`uint16_t`存储，导致移位后溢出为0
```c
// 错误 (uint16_t溢出):
uint16_t max_len = ((length - 1) & 0x7FF) << 21;  // 7 << 21 = 0x00E00000, 但uint16_t只能存储0

// 正确 (使用uint32_t):
uint32_t max_len = ((uint32_t)((length - 1) & 0x7FF)) << UHCI_TD_TOKEN_MAXLEN_SHIFT;
```

**修复**: 
- 所有TD max_len编码现在使用`uint32_t`
- Setup TD: `((uint32_t)0x7) << 21` = 8字节
- Data TD: `((uint32_t)(length-1) & 0x7FF) << 21`
- Status TD: `((uint32_t)0x7FF) << 21` = 零长度包

#### 2. TD链接指针Depth First位
**问题**: `UHCI_TD_LINK_DEPTH_FIRST`定义为`(0 << 0)`，实际应为`(1 << 2)`
```c
// 错误:
#define UHCI_TD_LINK_DEPTH_FIRST  (0 << 0)  // 等于0，无效果

// 正确:
#define UHCI_TD_LINK_DEPTH_FIRST  (1 << 2)  // Bit 2 = Depth First标志
```

#### 3. Status TD零长度包编码
**问题**: max_len设置为0，应该使用0x7FF表示零长度包
```c
// 错误:
((0) << UHCI_TD_TOKEN_MAXLEN_SHIFT)  // 表示1字节

// 正确:
((0x7FF) << UHCI_TD_TOKEN_MAXLEN_SHIFT)  // 表示零长度包
```

### QEMU UHCI源代码分析

通过分析QEMU `hcd-uhci.c`源代码，发现关键行为：

1. **TD token匹配**: `uhci_queue_fill`检查TD链中所有TD的token必须匹配（device address + endpoint）
2. **异步队列处理**: QEMU使用异步队列处理TD，不是同步完成
3. **TD链遍历**: 
   ```c
   while (is_valid(plink)) {
       if (!(ptd.ctrl & TD_CTRL_ACTIVE)) break;
       if (uhci_queue_token(&ptd) != q->token) break;  // Token必须匹配
       ...
   }
   ```

### 当前状态

**Setup TD成功**: 
- Status从`0x18800000`变为`0x18000007`
- ACTIVE清除，actual_length=7（8字节setup包）
- **但Data TD不被执行**

**Data TD问题**:
- Token现在正确: `0x00E80069` (PID=IN, maxlen=7, DATA1)
- 但ACTIVE位仍然为1，说明没有被处理
- QH element指向Data TD地址，说明硬件更新了element但没有处理

### 可能的问题

1. **QEMU UEFI模式UHCI模拟限制** - Setup后不继续处理TD链
2. **Token匹配问题** - Data TD token与Setup TD不完全匹配
3. **异步处理时序** - 轮询方式可能不适用于QEMU的异步模型

### 下一步计划

1. 尝试修改TD token确保完全匹配
2. 尝试使用QEMU EHCI控制器
3. 考虑使用XHCI控制器替代
4. 实现PS/2鼠标作为UEFI模式回退方案

## Previous UHCI Debugging Record

### Historical Status
- ✅ UHCI controller initialization successful
- ✅ Port successfully enabled (PED=1, CCS=1, status=0x85)
- ✅ Frame list 4KB aligned allocation
- ✅ Permanent control QH created
- ✅ FLBASE register correctly set
- ✅ Controller running (frame number increments)
- ❌ Control transfer still times out (2 seconds)
- ❌ TD status keeps ACTIVE bit (0x18800000), controller doesn't process transfers

## 已尝试的修复

### 1. 物理地址转换
- 创建`uhci_virt_to_phys()`函数
- 支持HHDM和堆地址转换
- 使用`vmm_get_physical()`遍历页表

### 2. 帧列表对齐
- 使用`pmm_alloc_pages(1)`分配4KB对齐物理内存
- 物理地址验证：< 4GB

### 3. 端口使能
- 强制设置PED位
- 端口状态从0x83 -> 0x81 -> 0x85

### 4. TD格式修复
- Setup TD: 不设置IOC位
- Data TD: 正确设置SPD位
- Status TD: 设置IOC位和DATA1切换位
- 错误计数器设置为3（bits 27-28）

### 5. 调度链实现
- 创建永久控制QH
- 所有帧列表项指向控制QH
- TD链正确链接

## 参考实现分析

### SerenityOS UHCI驱动
关键发现：
1. 创建完整的QH调度链：
   - `schedule_begin` -> `interrupt_qh` -> `ls_control_qh` -> `fs_control_qh` -> `bulk_qh`
2. 每个帧列表项指向Isochronous TD，再指向QH链
3. 使用`enqueue_qh()`将传输排队到锚点QH
4. 禁用中断（`write_usbintr(0)`）避免锁死

### Linux内核 UHCI驱动（uhci-hcd）
- 使用类似的调度链架构
- 支持异步/周期队列分离

## 可能的根本原因

### 1. QEMU UHCI模拟限制
- UEFI模式下UHCI模拟可能不完整
- 可能需要特殊的配置或初始化序列

### 2. TD/QH格式细节
- 可能需要特殊的位设置或对齐
- 数据切换位可能需要不同的处理

### 3. 调度链不完整
- 可能还需要创建其他QH（bulk, interrupt）
- 帧列表可能需要更复杂的结构

### 4. 控制器状态问题
- 可能需要额外的等待时间
- 可能需要特殊的控制器配置

## 下一步调试方向

### A. 参考SerenityOS完整实现
1. 创建完整的QH调度链
2. 实现`enqueue_qh()`和`dequeue_qh()`函数
3. 使用轮询方式检查传输完成

### B. 尝试不同的USB设备
- usb-mouse
- usb-kbd
- usb-storage

### C. 检查PCI配置空间
- 验证bus master已启用
- 检查中断线配置

### D. 实现PS/2鼠标备选方案
- 仅在BIOS模式下工作
- 作为USB失败的备选

## 调试日志摘要

### 成功的情况
```
[UHCI] Port 0 after manual enable: 0x85  (PED=1, CCS=1)
[UHCI] Control QH created at phys=0x3896E38
[UHCI] Setting frame list base address to 0x47C2000
[UHCI_CTRL] CMD=0xC1 STS=0x0 FRNUM=0x43C FLBASE=0x47C2000
```

### 失败的情况
```
[UHCI_CTRL] Waiting... STS=0x0 FR=1150
[UHCI_CTRL] Transfer TIMEOUT
[UHCI_CTRL] After wait - Setup TD status=0x18800000  (ACTIVE位保持)
```

## 关键代码位置
- UHCI驱动: `SYSTEM/lib/Ztknl/uhci.c`
- USB枚举: `SYSTEM/lib/Ztknl/usb.c`
- PCI探测: `SYSTEM/lib/Ztknl/usb_pci.c`

## 参考资源
- SerenityOS: https://github.com/SerenityOS/serenity
  - `Kernel/Bus/USB/UHCI/UHCIController.cpp`
  - `Kernel/Bus/USB/UHCI/UHCIRootHub.cpp`
- OSDev Wiki: https://wiki.osdev.org/Universal_Host_Controller_Interface_(USB_1.1)
- Intel UHCI规范
