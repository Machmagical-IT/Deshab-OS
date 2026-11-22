/* zts_elf.c — ZTS 模块正文加载器（ELF64 ET_REL）
 *
 * 支持的 section: SHF_ALLOC 的 PROGBITS / NOBITS。
 * 支持的 relocation（与 DKM 驱动加载器一致）:
 *   R_X86_64_NONE(0) / 64(1) / PC32(2) / PLT32(4) / 32(10) / 32S(11)
 * 未定义符号按名解析到 zts 导出符号表（zts_api 等）。
 */
#include "zts.h"

#define ELF_MAGIC      0x464C457FULL
#define ELF_ET_REL     1
#define ELF_MACHINE_X86_64 0x3E
#define ELF_SHT_PROGBITS 1
#define ELF_SHT_SYMTAB   2
#define ELF_SHT_NOBITS   8
#define ELF_SHT_RELA     4
#define ELF_SHF_ALLOC    0x2
#define ELF_SHN_UNDEF    0
#define ELF_SHN_ABS      0xFFF1

typedef struct __attribute__((packed)) {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr;

typedef struct __attribute__((packed)) {
    u32 name;
    u32 type;
    u64 flags;
    u64 addr;
    u64 offset;
    u64 size;
    u32 link;
    u32 info;
    u64 addralign;
    u64 entsize;
} elf64_shdr;

typedef struct __attribute__((packed)) {
    u32 name;
    u8 info;
    u8 other;
    u16 shndx;
    u64 value;
    u64 size;
} elf64_sym;

typedef struct __attribute__((packed)) {
    u64 offset;
    u64 info;
    i64 addend;
} elf64_rela;

#define ZTS_MAX_SHDR 96

typedef struct {
    u64 load;      /* 0 = 未装载 */
    u64 size;
    u64 file_off;
} sec_load;

static u64 sym_addr(const elf64_sym *s, const char *strtab, sec_load *secs, u32 shnum) {
    if (s->shndx == ELF_SHN_ABS) return s->value;
    if (s->shndx != ELF_SHN_UNDEF && s->shndx < shnum) {
        if (!secs[s->shndx].load) return 0;
        /* ET_REL: st_value 是 section 内偏移（sh_addr 通常为 0） */
        return secs[s->shndx].load + s->value;
    }
    if (s->shndx == ELF_SHN_UNDEF) {
        const char *nm = strtab + s->name;
        return zts_symbol_lookup(nm);
    }
    return 0;
}

static const char *sym_name(const elf64_sym *s, const char *strtab) {
    if (!s->name) return "";
    return strtab + s->name;
}

/* 把 ELF 正文装入 slot；返回 0 成功。入口偏移写入 init/exec/cleanup_off（相对正文文件偏移）。 */
int zts_elf_load(const u8 *body, u32 body_size, u32 slot, zts_module *out,
                 u64 *init_off, u64 *exec_off, u64 *cleanup_off) {
    if (!body || body_size < sizeof(elf64_ehdr)) return -1;
    const elf64_ehdr *eh = (const elf64_ehdr *)body;
    if (*(const u32 *)eh->ident != (u32)ELF_MAGIC) return -2;
    if (eh->ident[4] != 2 || eh->ident[5] != 1) return -2;     /* ELFCLASS64 / LE */
    if (eh->type != ELF_ET_REL) return -2;
    if (eh->machine != ELF_MACHINE_X86_64) return -2;
    if (!eh->shnum || eh->shentsize != sizeof(elf64_shdr)) return -2;
    if (eh->shoff + (u64)eh->shnum * sizeof(elf64_shdr) > body_size) return -2;
    if (eh->shnum > ZTS_MAX_SHDR) return -2;

    u8 *slot_base = (u8 *)zts_slot_base(slot);
    if (!slot_base) return -7;

    const elf64_shdr *sh = (const elf64_shdr *)(body + eh->shoff);
    sec_load secs[ZTS_MAX_SHDR];
    zts_memset(secs, 0, sizeof(secs));

    /* 1. 布局: 按 section 顺序分配 slot 内偏移（对齐到 addralign，最小 16） */
    u32 off = 0;
    for (u16 i = 1; i < eh->shnum; i++) {
        if (!(sh[i].flags & ELF_SHF_ALLOC)) continue;
        if (sh[i].type != ELF_SHT_PROGBITS && sh[i].type != ELF_SHT_NOBITS) continue;
        u64 align = sh[i].addralign ? sh[i].addralign : 16;
        if (align < 16) align = 16;
        off = (u32)((off + align - 1) & ~(align - 1));
        if (off + sh[i].size > ZTS_SLOT_SIZE) return -7;
        secs[i].load = (u64)(slot_base + off);
        secs[i].size = sh[i].size;
        secs[i].file_off = sh[i].offset;
        off = (u32)(off + sh[i].size);
    }
    if (!off) return -2;

    /* 2. 清零 slot（NOBITS 与对齐空洞） */
    zts_memset(slot_base, 0, ZTS_SLOT_SIZE);

    /* 3. 拷贝 PROGBITS */
    for (u16 i = 1; i < eh->shnum; i++) {
        if (!secs[i].load || sh[i].type == ELF_SHT_NOBITS) continue;
        if (sh[i].offset + sh[i].size > body_size) return -2;
        zts_memcpy((void *)secs[i].load, body + sh[i].offset, sh[i].size);
    }

    /* 4. 找 .symtab/.strtab */
    const elf64_sym *syms = 0;
    u32 sym_count = 0;
    const char *strtab = 0;
    for (u16 i = 1; i < eh->shnum; i++) {
        if (sh[i].type == ELF_SHT_SYMTAB) {
            if (sh[i].offset + sh[i].size > body_size) return -2;
            syms = (const elf64_sym *)(body + sh[i].offset);
            sym_count = (u32)(sh[i].size / sizeof(elf64_sym));
            if (sh[i].link < eh->shnum) {
                const elf64_shdr *st = &sh[sh[i].link];
                if (st->offset + st->size > body_size) return -2;
                strtab = (const char *)(body + st->offset);
            }
            break;
        }
    }

    /* 5. 应用 RELA */
    for (u16 i = 1; i < eh->shnum; i++) {
        if (sh[i].type != ELF_SHT_RELA) continue;
        u32 target = sh[i].info;
        if (target >= eh->shnum || !secs[target].load) continue;
        if (!syms || !strtab) return -4;
        if (sh[i].offset + sh[i].size > body_size) return -2;
        const elf64_rela *ra = (const elf64_rela *)(body + sh[i].offset);
        u32 count = (u32)(sh[i].size / sizeof(elf64_rela));
        for (u32 r = 0; r < count; r++) {
            u32 symidx = (u32)(ra[r].info >> 32);
            u32 type = (u32)(ra[r].info & 0xFFFFFFFFu);
            if (type == 0) continue;   /* R_X86_64_NONE */
            if (symidx >= sym_count) return -4;
            const elf64_sym *s = &syms[symidx];
            u64 S = sym_addr(s, strtab, secs, eh->shnum);
            if (!S && s->shndx == ELF_SHN_UNDEF) {
                const char *nm = sym_name(s, strtab);
                zts_log("[zts] unresolved symbol:");
                zts_log(nm);
                return -4;
            }
            u64 va = secs[target].load + ra[r].offset;
            if (va < (u64)slot_base || va + 8 > (u64)slot_base + ZTS_SLOT_SIZE) return -3;
            i64 A = ra[r].addend;
            switch (type) {
            case 1: {  /* R_X86_64_64 */
                *(u64 *)va = S + (u64)A;
                break;
            }
            case 2:    /* R_X86_64_PC32 */
            case 4: {  /* R_X86_64_PLT32 */
                i64 v = (i64)S + A - (i64)va;
                if (v < -2147483648LL || v > 2147483647LL) return -3;
                *(i32 *)va = (i32)v;
                break;
            }
            case 10: { /* R_X86_64_32 */
                u64 v = S + (u64)A;
                if (v > 0xFFFFFFFFULL) return -3;
                *(u32 *)va = (u32)v;
                break;
            }
            case 11: { /* R_X86_64_32S */
                i64 v = (i64)S + A;
                if (v < -2147483648LL || v > 2147483647LL) return -3;
                *(i32 *)va = (i32)v;
                break;
            }
            default:
                zts_log("[zts] unsupported relocation type");
                zts_log_dec("type=", type);
                return -3;
            }
        }
    }

    /* 6. 入口: 优先符号名，其次头内偏移 */
    u64 init_addr = 0, exec_addr = 0, cleanup_addr = 0;
    if (syms && strtab) {
        for (u32 i = 0; i < sym_count; i++) {
            const char *nm = sym_name(&syms[i], strtab);
            if (!nm[0] || syms[i].shndx == ELF_SHN_UNDEF) continue;
            u64 a = sym_addr(&syms[i], strtab, secs, eh->shnum);
            if (zts_str_eq(nm, "zts_module_init")) init_addr = a;
            else if (zts_str_eq(nm, "zts_module_exec")) exec_addr = a;
            else if (zts_str_eq(nm, "zts_module_cleanup")) cleanup_addr = a;
            /* 导出符号前缀 zts_export_*：登记到宿主符号表，供 patch/依赖解析 */
            else if (nm[0] == 'z' && nm[1] == 't' && nm[2] == 's' && nm[3] == '_' &&
                     nm[4] == 'e' && nm[5] == 'x' && nm[6] == 'p' && nm[7] == 'o' &&
                     nm[8] == 'r' && nm[9] == 't' && nm[10] == '_' && a) {
                zts_symbol_register(nm, a);
            }
        }
    }
    /* 头内偏移 -> 装载地址 */
    if (init_off && *init_off && !init_addr) {
        for (u16 i = 1; i < eh->shnum; i++) {
            if (!secs[i].load) continue;
            if (*init_off >= secs[i].file_off && *init_off < secs[i].file_off + secs[i].size) {
                init_addr = secs[i].load + (*init_off - secs[i].file_off);
                break;
            }
        }
    }
    if (exec_off && *exec_off && !exec_addr) {
        for (u16 i = 1; i < eh->shnum; i++) {
            if (!secs[i].load) continue;
            if (*exec_off >= secs[i].file_off && *exec_off < secs[i].file_off + secs[i].size) {
                exec_addr = secs[i].load + (*exec_off - secs[i].file_off);
                break;
            }
        }
    }
    if (cleanup_off && *cleanup_off && !cleanup_addr) {
        for (u16 i = 1; i < eh->shnum; i++) {
            if (!secs[i].load) continue;
            if (*cleanup_off >= secs[i].file_off && *cleanup_off < secs[i].file_off + secs[i].size) {
                cleanup_addr = secs[i].load + (*cleanup_off - secs[i].file_off);
                break;
            }
        }
    }

    out->code_base = slot_base;
    out->image_size = off;
    out->init = init_addr ? (int (*)(zts_api_table *))init_addr : 0;
    out->execute = exec_addr ? (int (*)(void *, void *, void *))exec_addr : 0;
    out->cleanup = cleanup_addr ? (int (*)(void))cleanup_addr : 0;
    return 0;
}
