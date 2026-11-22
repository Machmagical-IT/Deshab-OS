# ZTS 子系统（ZTS7 热更新 / 动态算法）

本目录是 Deshab 的 ZTS 模块工具链与示例源码。运行期实现在
`CODE/DKM/zts/`（DKM 驱动 `SYSTEM/driver/misc/zts.drv`），模块仓库在
`SYSTEM/zts/`（打包后为 ESP 根 `/zts/`）。

## 模块格式

```text
+----------------------------+ 0
| zts_header (760 bytes)     |  magic 0x53545A00, version=2
+----------------------------+ 760
| padding                    |  对齐到 16 字节（正文从 768 开始）
+----------------------------+ 768
| ELF64 ET_REL               |  未 strip 的重定位目标文件
+----------------------------+
```

头字段：name/desc/author/license/uuid/category/semver/依赖数/入口偏移/
checksum（FNV-1a 32，覆盖 ELF 正文）/signature（保留）。

正文 ELF 约束：

- `ET_REL`（`ld.lld -r`），x86_64，小端；
- 重定位：`R_X86_64_64/PC32/PLT32/32/32S`；
- 未定义符号按名解析到宿主导出表（`zts_api`、`zts_alloc`、`zts_memcpy` …）；
- 入口符号：`zts_module_init/exec/cleanup`（按名优先，头内偏移兜底）；
- 以 `zts_export_` 开头的已定义符号会登记到宿主符号表，可供
  `srv.zts.patch.install` 用 `sym:<name>` 定位。

## 构建

```powershell
# 由 build.ps1 自动执行；手工流程：
clang -target x86_64-unknown-none -ffreestanding -fno-builtin \
  -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel \
  -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -O2 \
  -I CODE/zts/include -c CODE/zts/modules/example_module.c -o example.o
ld.lld -r -o example.rel example.o
python CODE/zts/mkzts.py --in example.rel --out SYSTEM/zts/0x00000001.zts \
  --name example --uuid 00112233445566778899aabbccddeeff --category 9
```

`SYSTEM/zts/ZTS.CNF` 的每行声明一个自动加载模块：

```text
file|deps|autoload|required
0x00000001.zts||1|1
```

## 运行期入口

- Shell：`zts ls / load / unload / exec / update / rollback / ckpt / patch / diff / stats / state`
- 总线：`srv.zts.*`（list/load/unload/execute/hotupdate/rollback/checkpoint/diff/patch/state/stats/guest.status）
- 桌面：设置工具 ZTS 页（F4）
- 持久化：`/zts/STATEA.CNF`、`/zts/STATEB.CNF`（A/B + SEQ + CRC32）及
  `/zts/MxxxxxxA.ZTS`、`MxxxxxxB.ZTS` 模块副本。

## 热更新语义

1. 校验新模块头/ELF/校验和/依赖；
2. 创建检查点（模块注册表 + 槽位引用 + patch 表）；
3. 新模块装入新槽并执行 `init`；
4. 成功才切换注册表，随后调用旧模块 `cleanup`；
5. 持久化 A/B 状态，失败则回退并恢复检查点。

模块是外部输入：失败只回滚并打印 `ZTS-E##`，不会 panic 内核。
