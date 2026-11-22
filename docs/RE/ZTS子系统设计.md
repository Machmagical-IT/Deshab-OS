# ZTS 子系统设计（ZT5 内核代码 → Deshab 子系统）

> 源码母本：`CODE/ZT5/SYSTEM/lib/Ztknl/{hotupdate.c,zts_algorithm.c}`
> 运行期实现：`CODE/DKM/zts/` → `SYSTEM/driver/misc/zts.drv`
> 模块仓库：`SYSTEM/zts/` → 打包后 ESP 根 `/zts/`

## 0. 一句话

ZTS 把 ZT5 的热更新 / 动态算法子系统移植为 Deshab 的 DKM 模块：.zts 模块
以 ELF64 ET_REL 装载，经 `srv.zts.*` 服务总线提供加载、执行、热替换、
检查点回滚、差分更新、事务、函数替换与 A/B 持久化。

## 1. .zts 模块格式（v2）

```text
offset 0     : zts_header (760B, packed)
offset 760   : padding
offset 768   : ELF64 ET_REL body（16 字节对齐）
```

`zts_header` 字段与 ZT5 `zts_algorithm.h` 一致：

```c
uint32 magic = 0x53545A00; uint16 version = 2; uint16 api_min_version;
char name[64]; char description[256]; char author[64]; char license[32];
uint8 uuid[16]; uint32 category;
uint32 major, minor, patch; uint32 num_dependencies;
uint64 init_func_offset, cleanup_func_offset, execute_func_offset;
uint64 data_offset, data_size;
uint32 checksum;             /* FNV-1a 32 over ELF body */
uint8 signature[256];        /* 预留；当前策略见 §6 */
```

正文约束：

- ET_REL（`ld.lld -r`），x86_64；
- 重定位：`R_X86_64_NONE/64/PC32/PLT32/32/32S`；
- 未定义符号按名解析到宿主导出表：`zts_api`、`zts_alloc/free/realloc`、
  `zts_log`、`zts_time_ms`、`zts_memcpy/memset/strcmp/strlen/crc32`；
- 入口符号 `zts_module_init/exec/cleanup`（按名优先，头内偏移兜底）；
- `zts_export_*` 已定义符号登记到宿主符号表（供 patch/依赖检查）。

## 2. 模块内存与生命周期

- 槽位：32 × 128KB 静态 arena（DKM 驱动 .bss，UTSM arena 内 RWX）；
- 槽位不可变：热替换分配新槽，旧槽由检查点引用，引用归零才回收；
- 状态：LOADED → ACTIVE → UPDATING → ACTIVE/ROLLBACK；
- 卸载：调用 cleanup 后释放槽位引用。

## 3. 热更新流程

```text
verify header/checksum/deps → checkpoint_create
  → allocate new slot → zts_elf_load → new init()
  → registry swap → old cleanup() → persist state A/B
  → fail: rollback checkpoint + free new slot
```

## 4. 检查点 / 差分 / 事务 / 函数替换

- 检查点：注册表快照 + 槽位引用 + patch 表（8 个槽）；
- 差分：`DIFF` 头 + 4096B 定长块，支持 modify/insert/delete；
- 更新包 `.zup`：`ZUP1` 头 + 全量模块或差分数据；应用后走热替换；
- 事务：内存/文件/模块三类 op，提交顺序执行，回滚逆序恢复；
- 函数替换：5 字节 `E9 rel32` 或 14 字节 `FF 25 abs64`；保存 16 字节原始
  指令；版本快照可整体回滚。

## 5. 持久化

```text
/zts/ZTS.CNF          清单: file|deps|autoload|required
/zts/STATEA.CNF       状态槽 A（SEQ + MOD|... + CRC32）
/zts/STATEB.CNF       状态槽 B
/zts/M<6hex>A.ZTS     A 侧模块副本（8.3）
/zts/M<6hex>B.ZTS     B 侧模块副本
```

开机：读两个状态槽，选 SEQ 最大且 CRC 有效者；按其中 `file` 加载模块副本。
热更新成功后才写非活动槽，读回校验通过则切换。

## 6. 校验与签名

- checksum：FNV-1a 32（头字段），必选；
- signature：当前仅接受全零（未签名）；非零签名按 `ZTS-E11` 拒绝
  （ZT5 的 RSA/ECDSA 分支从未实现，不做静默放行）。
- 失败策略：模块是外部输入，校验/加载/init 失败 → 回滚 + `ZTS-E##` 日志，
  不 panic 内核；内部不变量破坏走 `ZTS-E90` panic。

## 7. 错误码

```text
ZTS-E01 模块头非法        ZTS-E02 ELF 非法
ZTS-E03 重定位不支持      ZTS-E04 符号未解析
ZTS-E05 完整性校验失败    ZTS-E06 依赖缺失
ZTS-E07 槽位耗尽          ZTS-E08 检查点不存在
ZTS-E09 差分包非法        ZTS-E10 状态写失败
ZTS-E11 签名非法          ZTS-E12 模块执行失败
ZTS-E13 状态槽非法        ZTS-E14 服务参数非法
ZTS-E90 内部不变量破坏
```

## 8. 服务总线

```text
srv.zts.list(char *buf, u32 cap) -> int count
srv.zts.load(const char *file)
srv.zts.unload(const char *name)
srv.zts.execute(const char *args)          // "name"
srv.zts.hotupdate(const char *args)        // "name|file"
srv.zts.rollback(const char *args)         // "ckpt:<id>" | "ver:<v>" | name
srv.zts.checkpoint.create(const char *desc) -> int id
srv.zts.checkpoint.restore(u64 id)
srv.zts.diff.apply(const char *pkg)
srv.zts.patch.install(const char *args)    // "target|replacement|tname|rname"
srv.zts.patch.uninstall(const char *args)  // "target"
srv.zts.state(char *buf, u32 cap)
srv.zts.stats(char *buf, u32 cap)
srv.zts.guest.status(char *buf, u32 cap)   // 转发 kern.zt5.status
```

地址参数支持 `0x...` 或 `sym:<导出符号名>`。

## 9. 与 ZT5 guest 的关系

`SYSTEM/zts` 同时是 ZT5 guest 的模块源：EFI 引导器把 ZTS.CNF 与其列出的
`.zts` 预加载为 `zts:*` boot module，UTSM 的 zt5_loader 把它们转成 guest
的 Limine module（路径 `/zrr64/zts/p.zts6/*.zts`）。guest 内 ZT5 的
`zts_algorithm.c` 完成从 boot module 加载；宿主侧 zts.drv 负责持久化与
A/B 回滚。运行中 guest 内核镜像不做热替换，只做 A/B 下次启动生效。

### 9.1 Guest 装载实现（UTSM VMX）

- `zt5_loader.c`：ELF ET_EXEC 解析 → boot area/fb/modules/kernel(64MB)/RAM(1GB)
  分配 → guest 页表（恒等 0-4GB + HHDM + 高半区，**PDPT_K 索引 510**）
  → GDT → 伪 Limine 响应 → `.limine_reqs` 回填（无 section 头时扫描前 64KB）
  → EPT 映射。
- `zt5_boot.c`：VPID=4；guest CR0 含 MP/ET、CR4 含 VMXE；selector=0 的
  FS/GS/LDTR 标 unusable；MSR bitmap 物理地址用 `vmx_kernel_virt_to_phys`；
  vmlaunch 前 VMCLEAR+VMPTRLD。
- Guest 可见结构中的指针必须是 **guest HHDM VA**（GPA + 0xFFFF800000000000），
  不能写 host VA（否则 `framebuffers[0] == NULL` / 模块表指针错）。
- 不得 EPT identity-map 低 1MB（会覆盖 boot area 0x20000 的页表映射）。
- VM-exit 适配：HLT 推进 RIP；PIT tick 以 vector 48 注入（ZT5
  `apic_timer_handler → scheduler_tick`）；ATA 状态端口返回 ERR 让磁盘探测
  快速失败；COM1 复用既有模拟。

### 9.2 验证状态（2026-10-18）

WSL2 + KVM（nested VMX）下 ZT5 guest 已启动到：
`_start` → GDT/IDT → PMM/VMM/Heap → APIC/SMP → input/PS2 → disk 探测失败
回退 RAMFS → VFS → Limine 模块解包 → ZTS7/AI 初始化 → Stage 9 →
加载 `/prgm/sys/desk` → scheduler_start。

**剩余阻塞**：scheduler 后 guest 页错误（desktop 返回路径），待 guest
build profile（PIT tick / 跳过 APIC/SMP/磁盘）与用户态页表继续定位。
当前 `[zt5] mode=headless` 为独占运行，host 在 guest 终止后继续。
