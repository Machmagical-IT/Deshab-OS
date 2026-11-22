# Deshab-OS 任务板

> 按 [ROADMAP_INSANE.md](./ROADMAP_INSANE.md) 五期编号的可勾选任务表。
> 状态：✅ 已实现 · 🔶 演示级 · ⬜ 仅设计。唯一状态源见 [STATUS.md](./STATUS.md)。

## 🔩 一期：骨架

- [x] F1 内核 panic 统一兜底（莲花崩溃屏 + panic_symbol + DRR 归档）
- [x] F4 boot_ok 分级启动自检（`[boot] selftest=0|1|2`）
- [x] F5 可跳过 FirstInit（`[dsk] skip_firstinit` + build/configs 模板）
- [x] F2 统一帧缓冲控制台抽象（fbcon.h：panic 路径已用；统一控制台后端待做）
- [x] F3 跨平台构建（根 Makefile：make dev/release/realtest；macOS 打包受 mkfs.ext4 限制）

## 🗝️ 二期：仪式（UTSM/UTRW）

- [x] U1 UTSM 元数据区 / UUID 调谐表
- [x] U2 UTRW Slow Path 真分支（EPOCH/KEY_MISS 恢复 + MAC_FAILED/POISONED→DRR；selftest PASS）
- [x] U3 `@sealed` 段可选标记（`UTSM_SEG_F_UNSEALED` 明文直存段 + linker.ld `.sealed` section；selftest PASS）
- [x] U4 密文页 dump 工具（utrw_debug_dump_page：密文/明文 hex 对照，selftest 演示 PASS）
- [x] U5 UTSM selftest 全覆盖（RFC 8439 KAT 落地，"间歇 FAIL" 确认为幽灵缺陷；全量稳定 PASS）

## 🔌 三期：自由（DKM/用户态）

- [x] D5 最小 REPL/shell（shell.elf 25+ 命令）
- [ ] D1 DKM 热卸载（引用计数 + 资源回收，循环 100 次不崩）
- [ ] D2 裸机器码加载器
- [ ] D3 内核 API 按名导出表 → 双向服务总线（kapi 只读表 ✅；dkm_bus 注册-消费进行中，见 D6）
- [x] D4 DKM 彩色日志前缀（驱动名 hash → 8 色 ANSI，init 期着色，BOOTLOG 保持纯文本）

## 🔗 三期扩展：万物互联（D6/D7，P0-P6）

> 设计定稿：[RE/万物互联与服务总线设计.md](./RE/万物互联与服务总线设计.md)——没有主内核，一切皆 DKM 模块，模块间经按名服务总线互联。

- [ ] P0 manifest 依赖图强制（depends 校验 + 不动点排序，DKM-E44/E47；负向测试莲花屏）
- [ ] P1 dkm_bus 服务总线（kern.*/srv.* 双向注册-消费、BUS-E01..05、provider 上下文、DKM-E45 provides 校验、/dev/services、TST-E25..28）
- [ ] P2 fat32.drv 实装 + ext4.drv（srv.fs.fat32.* / srv.fs.ext4.*；首次启动全流程不回归）
- [ ] P3 加载器 FS 化 + stage4 session + ps2mouse.drv（dsm_read_module_fs、DKM-E46、kern.dkm.load_by_name、srv.input.mouse.poll）
- [ ] P4a netman.drv（srv.net.lease）
- [ ] P4b shell.drv（probe/lease/dev_mode 走总线）
- [ ] P4c firstinit.drv（user.conf 写盘收编，reserved[0/1] 删除，srv.user.config）
- [ ] P4d login.drv（自读 USER.CONF，srv.auth.result）
- [ ] P4e desktop.drv（~20TU ld -r，host SDL #ifdef 隔离，readobj 重定位核对）
- [ ] P4f cmd.drv + 收尾（DSK 只剩排序骨架）
- [ ] P5 DSK 退役 + session.drv（删 deshab.elf/dsk_loader.c/dsk_boot_context；dev/release 双过）
- [x] P6 常驻调度 + 真 IRQ 输入（sched_enable 常驻 + bootstrap park、desktop 任务化、IRQ1/12 解屏蔽、kbd 事件环）
  - ✅ P6a 调度器常驻 + ps2kbd IRQ1 ring + `srv.input.kbd.poll/.snapshot`
  - ✅ P6b 输入总线化：firstInit/login/shell/cmd/desktop 全部改消费 `srv.input.kbd.poll` / `srv.input.mouse.poll`；`kern.irq.unmask` 解屏蔽 IRQ1/12 + IF=1；业务代码 `inb(0x60)` 归零
  - ✅ P6b-desktop 任务化：`srv.desktop.spawn` 建任务 slot=1，每帧 `sleep_ms(1)`；session 返回后 bootstrap 转 idle；证据 `p6c_final.log`
  - 证据 `.build_tmp/p6b_qemu.log` + `.build_tmp/p6c_final.log`：`[session] IF=1 (true IRQ input armed)`、`[firstinit] kbd event via srv.input.kbd.poll`、`[login] password correct`、`[desktop] desktop ready`、`[desktop] kbd event via srv.input.kbd.poll`、`mouse first pkt via srv.input.mouse.poll`
- [ ] D7 收尾（kapi 静态表退役、kern.block.* 消费者切换、STUB 基线复核）

## ⏪ 四期：落款（DRR/PCQ）

- [x] R1 checkpoint 快照（A/B 双槽 + CRC64 + 原子切换）
- [x] R2 看门狗（timeout 检出 + 回滚/复位，负向测试 PASS）
- [x] R3 Emergency Pool（64KB 编译期独立池）
- [x] R3b `drr_emergency_pool_kb` / `drr_recovery_log_size` FUCK 键接线（运行期钳位到编译期上限）
- [x] R4 A/B 回滚收尾（segment/system 级 ✅，真 BLAKE2b 页 MAC；A/B 槽破坏 fallback 硬验收 PASS，`.build_tmp/qemu_serial_r4ab.log`）
- [ ] R5 PCQ 多核（per-cpu runqueue 负载均衡；BSP-only 🔶）

## 🪷 五期：品牌

- [x] E5 疯圣典进 docs/（PHILOSOPHY.md）
- [ ] E1 启动莲花动画
- [ ] E2 "假想时光键"彩蛋
- [ ] E3 提交信息规范（疯洁版）
- [ ] E4 deshab-fuckyou-edition 风味 ISO

---

## 遗留工程债（Phase 存档，非五期编号）

- [x] Vector allocator / MSI-X（NVMe 使用 IDT 向量池 0x40–0xDF + MSI-X 表项编程）
- [x] NVMe 高位 BAR MMIO（BAR0>4G，`mm_map_mmio` 独立窗口映射）
- [x] virtio-net virtqueue RX/TX（legacy split-ring + DHCP selftest）
- [ ] FAT32 文件系统 API 层（DKM 侧当前只有 BPB/目录 demo，无 open/read/stat）
- [ ] DKM 零 warning + 统一 ABI 头
