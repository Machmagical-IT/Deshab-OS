# CODE/third_party — 第三方源码

本目录只放**可直接编译进 Deshab**（freestanding / 无 libc 依赖或依赖可裁剪）
的第三方源码。带完整构建系统、需要 POSIX/libc 的重量级库（mbedTLS、Lexbor）
**不入库**，需要时从上游取；见文末"未纳入"一节。

来源均为 `SYSTEMCOPY/OpenXJ380/third_party/`（同项目 Apache-2.0 兼容筛选后的
vendored drops），原始许可证文件随目录保留。

| 目录 | 上游 | 许可证 | 用途 |
|---|---|---|---|
| `libvterm/` | [libvterm](https://github.com/neovim/libvterm)（Paul Evans） | MIT（`libvterm/LICENSE`） | 终端转义序列 → 屏幕网格（VT100/ECMA-48 状态机 + 宽字符支持）。Deshab 的 shell/浏览器终端页可用它替换手写 ANSI 解析 |
| `libutf/` | Karl Williamson / Markus Kuhn UTF-8 参考实现 | MIT（`libutf/LICENSE`、`libutf/SOURCE.md`） | UTF-8 编解码表与辅助宏（`CODE/desktop/zhfont.c` 与 browser 的 UTF-8 走查可复用） |
| `mikanos-hankaku/` | [MikanOS](https://github.com/uchan-nos/mikanos) | Apache-2.0（`mikanos-hankaku/LICENSE`） | 8×16 半角字形源文本（`hankaku.txt`）+ 校验哈希；用于生成独立于 Consolas 位图的紧凑 ASCII 字库（引导器/panic 路径备选） |
| `dr_mp3.h` | [dr_libs](https://github.com/mackron/dr_libs)（David Reid） | MIT-0 OR Unlicense（文件末尾许可见文） | 单头 MP3 解码器；接 `srv.audio.play`（hda.drv）即可播放 MP3 |

## 使用约定

- **libvterm**：仅编 `src/{vterm,parser,state,screen,pen,keyboard,mouse,encoding,unicode}.c`
  + `src/encoding/*`。它 include `<stdio.h>/<stdlib.h>/<string.h>`，Deshab 侧需提供
  一个极小 shim（`memcpy/memset/strlen/malloc/free/snprintf`）。不要 `#include`
  `encoding/*.inc` 之外的文件系统相关代码（`bin/`、`t/` 是测试与工具，不编）。
- **libutf**：`SOURCE.md` 指向的上游表已内联；`CODE/desktop/zhfont.c` 的手写
  UTF-8 解码保持现状（无外部符号依赖）。新代码如要完整 UTF-8（含 5/6 字节序列
  校验、BOM 处理）优先引本目录，不再另写。
- **hankaku**：`hankaku.txt` 是文本源，运行时**不要**直接解析它（体积与
  解析成本都不合适）；`CODE/font/mkfont.py` 生成 `.dbf` 才是正路。本目录
  保留它只作为字形来源与回归比对的基准。
- **dr_mp3**：单 TU 包含（`#define DR_MP3_IMPLEMENTATION` 前须先给
  `malloc/free` 的 freestanding 实现）。输出 16-bit PCM 直接喂
  `srv.audio.play(pcm, bytes, rate, channels, 16)`。

## 未纳入（需从上游取）

| 库 | 原因 |
|---|---|
| **mbedTLS**（`SYSTEMCOPY/OpenXJ380/third_party/mbedtls-src`，~8MB） | 需要 POSIX + 完整 libc（`stdio`/`time`/`pthread` shim 跨十几个文件），Deshab 目前无 libc；HTTPS 走 UTSM 的 Linux guest（`kern.lxc.service` 里 guest 的 curl 已能做 TLS）。真要做原生 TLS 再引，届时按上游 `configs/config-suite-b.h` 裁剪到 ECDHE-RSA-AES128-GCM-SHA256 单套件 |
| **Lexbor**（`SYSTEMCOPY/OpenXJ380/third_party/lexbor`，22MB） | 完整 HTML/CSS 解析器，依赖自身构建图与 `lexbor/ports`；Deshab 浏览器目前只做"移除标签"的纯文本显示，引入完整 DOM 应作为独立任务（先定 layout 引擎） |
| **BusyBox**（2.4MB 预编译 + 源码包） | GPL-2.0，且只对 Linux guest 有意义；Deshab 主系统不链 GPL 代码。guest initramfs 已用 busybox.net 官方 musl 静态版（`CODE/linux/build.sh` 下载） |

## 许可证合规

- 复制自 Apache-2.0 项目的**文件**保留原许可证；本项目 `LICENSE` 为 Apache-2.0，
  与本目录各库兼容（MIT / MIT-0 / Apache-2.0 / Unlicense 均可再分发）。
- 修改第三方源码时在同目录 `PATCHES.md` 记录 diff 摘要，不改原 LICENSE。

## 目录裁剪记录

`libvterm/` 已删除上游的 `bin/`（vterm-dump 等工具）、`t/`（测试套件）、
`doc/`、`.github/`——它们只服务上游自己的构建，Deshab 不链接。保留
`src/`、`include/`、`LICENSE`、`README.md`、`Makefile`（仅作参考，不参与
Deshab 构建）与两个 `*.pl`（`fullwidth.inc` 的再生成脚本）。
