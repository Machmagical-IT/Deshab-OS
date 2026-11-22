# Rust自愈内核 (Rust Self-Healing Kernel)

基于ZratroOS自愈框架的完整Rust实现。

## 项目结构

```
rust_selfheal_kernel/
├── Cargo.toml              # Rust项目配置
├── src/
│   ├── lib.rs              # 库入口和模块导出
│   ├── core/
│   │   ├── types.rs        # 核心类型定义 (FaultType, Severity等)
│   │   └── manager.rs      # SelfHealManager实现
│   ├── handlers/
│   │   └── mod.rs          # 故障处理器系统和内建处理器
│   ├── policies/
│   │   └── mod.rs          # 恢复策略 (默认、保守、激进)
│   ├── watchdog/
│   │   └── base.rs         # 看门狗和WatchdogManager
│   ├── checkpoint/
│   │   └── base.rs         # 检查点管理
│   ├── dual_kernel/
│   │   └── base.rs         # 双内核通信
│   └── utils/
│       ├── sync.rs         # 同步原语 (SpinLock, RwLock)
│       └── mod.rs          # 工具模块和日志
├── tests/
│   └── integration_tests.rs # 集成测试
└── examples/
    └── demo.rs             # 演示应用
```

## 核心特性

### 1. 故障管理系统
- **故障类型**: 支持19种故障类型 (OOM、死锁、硬件故障等)
- **严重级别**: 6个严重级别 (Info到Fatal)
- **故障上下文**: 完整的故障信息捕获

### 2. 自愈管理器 (SelfHealManager)
- 故障报告和跟踪
- 故障处理器注册和管理
- 自动恢复执行
- 隔离域管理
- 统计数据收集

### 3. 故障处理器
内置处理器:
- **MemoryFaultHandler**: 处理内存相关故障
- **HardwareFaultHandler**: 处理硬件故障
- **SyncFaultHandler**: 处理同步故障 (死锁、活锁)
- **GenericFaultHandler**: 通用处理器

### 4. 恢复策略
- **默认策略**: 基于故障严重级别选择恢复动作
- **保守策略**: 优先选择较安全的恢复动作
- **激进策略**: 优先选择快速恢复动作

### 5. 看门狗系统
- 可配置超时的看门狗定时器
- 看门狗管理器支持多看门狗
- 超时检测和回调机制

### 6. 检查点系统
- 系统状态保存和恢复
- 检查点管理
- 支持回滚恢复

### 7. 双内核通信
- 内核间消息传递
- 共享内存接口
- 心跳检测

### 8. 同步原语
- SpinLock: 自旋锁
- RwLock: 读写锁
- Arc: 原子引用计数

## 使用方法

### 基本用法

```rust
use rust_selfheal_kernel::core::{
    FaultContext, FaultType, Severity, SelfHealManager,
};

fn main() {
    // 创建并初始化自愈管理器
    let manager = SelfHealManager::new();
    manager.init().expect("Failed to initialize");

    // 报告故障
    let fault = FaultContext::new(FaultType::OutOfMemory, Severity::Major)
        .component("memory_manager")
        .message("System out of memory");

    match manager.report_fault(fault) {
        Ok(action) => println!("Recovery action: {:?}", action),
        Err(e) => eprintln!("Recovery failed: {:?}", e),
    }
}
```

### 运行测试

```bash
cd rust_selfheal_kernel
cargo test
```

### 运行演示

```bash
cd rust_selfheal_kernel
cargo run --example demo
```

## 与ZratroOS集成

此Rust自愈内核实现与ZratroOS的C语言自愈框架(selfheal.c/h)兼容:

- 相同的故障类型定义
- 兼容的严重级别
- 一致的恢复策略
- 可以共存或互相替代

## 许可证

MIT OR Apache-2.0
