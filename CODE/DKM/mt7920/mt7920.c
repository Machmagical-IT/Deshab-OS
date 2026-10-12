/* DKM mt7920 — MediaTek MT7920 / Wi-Fi 6 (PCI 14c3:7920), ConnAC2 PCIe
 *
 * 本机无线网卡。驱动分阶段落地，当前阶段 = MCU 固件下载（patch + RAM），
 * 到 "N9 firmware ready" 为止。扫描/关联仍为显式 panic（MT7920-E10/E11），
 * 不伪装成功。
 *
 * 固件通过 Limine boot module 送入（limine.conf fw:mt7920_patch / fw:mt7920_ram），
 * 驱动用 boot_modules_response 按 cmdline 定位，不嵌进 .drv。
 *
 * 寄存器/流程对拍 Linux mt76（drivers/net/wireless/mediatek/mt76），
 * 版本见 .build_tmp/mt76ref。全程串口日志，便于实机逐步 bring-up。
 *
 * 这是硬件 bring-up 第一阶段：本环境（QEMU 无此卡）无法验证，需实机串口迭代。
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

/* ---- limine boot module 结构（与 bootfs.drv 一致） ---- */
struct limine_file {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8 gpt_disk_uuid[16];
    u8 gpt_part_uuid[16];
    u8 part_uuid[16];
};
struct limine_module_response {
    u64 revision;
    u64 module_count;
    struct limine_file **modules;
};

static const char *const g_depends[] = {"pci"};
static const char *const g_provides[] = {"netdev"};
__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC,.abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="mt7920",.version="0.2.0",.vendor="Deshab",
    .driver_class=9,.stage=3,.flags=0,.priority=0,
    .depends=g_depends,.depends_count=1,
    .provides=g_provides,.provides_count=1,.min_kernel_abi=1,
};

#define MT_VID 0x14C3u
#define MT_DID 0x7920u

/* ===== 寄存器（对拍 mt792x_regs.h / mt7921/regs.h，物理地址） ===== */
#define MT_HW_CHIPID            0x70010200u
#define MT_HW_REV               0x70010204u
#define MT_HW_BOUND             0x70010020u
#define MT_TOP_LPCR_HOST_BAND0  0x18060010u
#define MT_TOP_LPCR_HOST_FW_OWN  (1u<<0)
#define MT_TOP_LPCR_HOST_DRV_OWN (1u<<1)
#define MT_WFSYS_RST            0x18000140u
#define WFSYS_SW_RST_B          (1u<<0)
#define WFSYS_SW_INIT_DONE      (1u<<4)

/* conn_host_csr_top 0x7c060000 → BAR0 0xe0000 */
#define MT_CONN_ON_LPCTL        0x0e0010u
#define PCIE_LPCR_HOST_SET_OWN  (1u<<0)
#define PCIE_LPCR_HOST_CLR_OWN  (1u<<1)
#define PCIE_LPCR_HOST_OWN_SYNC (1u<<2)
#define MT_CONN_ON_MISC         0x0e00f0u
#define MT_TOP_MISC_FW_STATE    0x7u      /* GENMASK(2,0) */
#define MT_TOP_MISC2_FW_PWR_ON  (1u<<0)
#define MT_TOP_MISC2_FW_N9_RDY  0x3u      /* GENMASK(1,0) */

/* WFDMA0 直接 BAR0 偏移（base 0xd4000） */
#define MT_WFDMA0(ofs)          (0xd4000u + (ofs))
#define MT_WFDMA0_RST           MT_WFDMA0(0x100)
#define MT_WFDMA0_RST_LOGIC_RST       (1u<<4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST (1u<<5)
#define MT_MCU_CMD              MT_WFDMA0(0x1f0)
#define MT_WFDMA0_GLO_CFG       MT_WFDMA0(0x208)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN   (1u<<0)
#define MT_WFDMA0_GLO_CFG_TX_DMA_BUSY (1u<<1)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN   (1u<<2)
#define MT_WFDMA0_GLO_CFG_RX_DMA_BUSY (1u<<3)
#define MT_WFDMA0_GLO_CFG_TX_WB_DDONE (1u<<6)
#define MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN (1u<<12)
#define MT_WFDMA0_GLO_CFG_RX_WB_DDONE (1u<<13)
#define MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN (1u<<15)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO (1u<<28)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2 (1u<<21)
#define MT_WFDMA0_GLO_CFG_CLK_GAT_DIS (1u<<30)
#define MT_WFDMA0_GLO_CFG_DMA_SIZE_SHIFT 4
#define MT_WFDMA0_RST_DTX_PTR   MT_WFDMA0(0x20c)
#define MT_WFDMA0_PRI_DLY_INT_CFG0 MT_WFDMA0(0x2f0)
#define MT_WFDMA0_GLO_CFG_EXT0  MT_WFDMA0(0x2b0)
#define MT_WFDMA0_GLO_CFG_EXT0_CSR_TX_DMASHDL_EN (1u<<6)
#define MT_DMASHDL_SW_CONTROL   (0x7c026004u) /* via fixed map below */
#define MT_DMASHDL_DMASHDL_BYPASS (1u<<28)
#define MT_WFDMA0_HOST_INT_ENA  MT_WFDMA0(0x204)
#define MT_WFDMA0_HOST_INT_STA  MT_WFDMA0(0x200)
#define MT_MCU2HOST_SW_INT_ENA  MT_WFDMA0(0x1f4)
#define MT_WFDMA_DUMMY_CR       (0x54000120u)
#define MT_WFDMA_NEED_REINIT    (1u<<1)

#define MT_TX_RING_BASE         MT_WFDMA0(0x300)
#define MT_RX_EVENT_RING_BASE   MT_WFDMA0(0x500)
#define MT_RX_DATA_RING_BASE    MT_WFDMA0(0x520)
#define MT_WFDMA0_TX_RING0_EXT_CTRL MT_WFDMA0(0x600)
#define MT_RING_SIZE            0x10u

#define MT_PCIE_MAC_INT_ENABLE  0x010188u  /* PCIE_MAC_IREG 0x74030000→0x10000 +0x188 */

/* L1 remap（高位地址窗口），MT_INFRA 0xfe000 */
#define MT_HIF_REMAP_L1         0x0fe24cu
#define MT_HIF_REMAP_BASE_L1    0x40000u

/* ConnAC2 队列索引 */
#define TXQ_FWDL   16
#define TXQ_MCU_WM 17
#define RXQ_MCU_WM 0

/* MCU 命令 */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ 0x01
#define MCU_CMD_FW_START_REQ           0x02
#define MCU_CMD_NIC_POWER_CTRL         0x04
#define MCU_CMD_PATCH_START_REQ        0x05
#define MCU_CMD_PATCH_FINISH_REQ       0x07
#define MCU_CMD_PATCH_SEM_CONTROL      0x10
#define MCU_CMD_FW_SCATTER             0xee

#define PATCH_SEM_RELEASE 0
#define PATCH_SEM_GET     1
#define PATCH_NOT_DL_SEM_SUCCESS 2
#define PATCH_IS_DL              3
#define PATCH_REL_SEM_SUCCESS    4

#define DL_MODE_NEED_RSP  (1u<<31)
#define FW_START_OVERRIDE (1u<<0)
#define FW_FEATURE_OVERRIDE_ADDR (1u<<5)
#define FW_FEATURE_NON_DL        (1u<<6)
#define PATCH_SEC_TYPE_MASK 0xffffu
#define PATCH_SEC_TYPE_INFO 0x2

#define MCU_PKT_ID 0xa0u
#define FWDL_MAX_LEN 4096

/* ===== 固件头结构（big-endian patch header，little-endian ram trailer） ===== */
struct patch_hdr {
    char build_date[16];
    char platform[4];
    u32 hw_sw_ver;   /* BE */
    u32 patch_ver;   /* BE */
    u16 checksum;
    u16 rsv;
    struct {
        u32 patch_ver;
        u32 subsys;
        u32 feature;
        u32 n_region;   /* BE */
        u32 crc;
        u32 rsv[11];
    } desc;
};
struct patch_sec {
    u32 type;       /* BE */
    u32 offs;       /* BE */
    u32 size;       /* BE */
    u32 addr;       /* BE (info.addr) */
    u32 len;        /* BE (info.len) */
    u32 sec_key_idx;
    u32 align_len;
    u32 rsv[9];
};
struct fw_trailer {
    u8 chip_id;
    u8 eco_code;
    u8 n_region;
    u8 format_ver;
    u8 format_flag;
    u8 rsv[2];
    char fw_ver[10];
    char build_date[15];
    u32 crc;
};
struct fw_region {
    u32 decomp_crc;
    u32 decomp_len;
    u32 decomp_blk_sz;
    u8 rsv[4];
    u32 addr;       /* LE */
    u32 len;        /* LE */
    u8 feature_set;
    u8 type;
    u8 rsv1[14];
};

/* mt76 descriptor */
struct mt_desc { u32 buf0; u32 ctrl; u32 buf1; u32 info; };
#define MT_DMA_CTL_SD_LEN0_SHIFT 16
#define MT_DMA_CTL_LAST_SEC0 (1u<<30)
#define MT_DMA_CTL_DMA_DONE  (1u<<31)

/* ===== 驱动状态 ===== */
static struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;
static volatile u8 *g_bar;         /* BAR0 虚拟基址 */
static u8 g_bus, g_dev, g_func;
static u8 g_mcu_seq;

struct mt_queue {
    struct mt_desc *desc;   /* 虚拟 */
    u64 desc_phys;
    u32 ndesc;
    u32 head;
    u32 ring_base;  /* 寄存器基 */
    u32 hw_idx;     /* ring index within base */
};
static struct mt_queue g_q_fwdl, g_q_wm, g_q_rxmcu;
static struct dkm_dma_buffer g_tx_scratch;    /* TX 命令/scatter 暂存 */
static struct dkm_dma_buffer g_rx_bufs;       /* RX buffer 池 */
#define RX_NDESC 8u
#define TX_SCRATCH_SZ 8192u

/* ===== 串口日志 ===== */
#define COM1 0x3F8
static void rlog(const char *s){ while(*s){char c=*s++; if(c=='\n'){dkm_serial_wait_tx(COM1);dkm_outb(COM1,'\r');} dkm_serial_wait_tx(COM1);dkm_outb(COM1,(u8)c);} }
static void rhex(const char *p, u64 v){ static const char hx[]="0123456789abcdef"; char b[19]; rlog(p); b[0]='0';b[1]='x'; for(int i=0;i<16;i++)b[2+i]=hx[(v>>((15-i)*4))&0xf]; b[18]=0; rlog(b); rlog("\n"); }

/* strict 模式 g_log->panic 不返回；fallback 模式返回，调用点随后 return -1 */
static void mt_panic(const char *sym){ if(g_log)g_log->panic(sym); else for(;;)__asm__ volatile("cli;hlt"); }

/* ===== 基础 MMIO ===== */
static inline u32 bar_rr(u32 off){ return *(volatile u32*)(g_bar+off); }
static inline void bar_wr(u32 off,u32 v){ *(volatile u32*)(g_bar+off)=v; }
static inline void bar_set(u32 off,u32 m){ bar_wr(off, bar_rr(off)|m); }
static inline void bar_clr(u32 off,u32 m){ bar_wr(off, bar_rr(off)&~m); }

/* 固定映射：把高位物理地址翻成 BAR0 偏移（对拍 __mt7921_reg_addr 子集） */
static u32 fixed_map(u32 addr){
    struct { u32 phys, maps, size; } m[] = {
        {0x54000000,0x02000,0x01000},
        {0x55000000,0x03000,0x01000},
        {0x7c000000,0xf0000,0x10000},
        {0x7c020000,0xd0000,0x10000},
        {0x7c060000,0xe0000,0x10000},
        {0x74030000,0x10000,0x10000},
    };
    if(addr<0x100000) return addr;
    for(u32 i=0;i<sizeof(m)/sizeof(m[0]);i++){
        if(addr<m[i].phys) continue;
        u32 ofs=addr-m[i].phys;
        if(ofs>m[i].size) continue;
        return m[i].maps+ofs;
    }
    return 0; /* 需 L1 remap */
}

/* L1 remap 访问高位寄存器（0x18xxxxxx / 0x70xxxxxx 等） */
static u32 l1_addr(u32 addr){
    u32 base=addr>>16, offset=addr&0xffff;
    u32 cur=bar_rr(MT_HIF_REMAP_L1);
    bar_wr(MT_HIF_REMAP_L1,(cur&~0xffffu)|(base&0xffffu));
    (void)bar_rr(MT_HIF_REMAP_L1);
    return MT_HIF_REMAP_BASE_L1+offset;
}
static u32 reg_rr(u32 phys){ u32 o=fixed_map(phys); if(!o)o=l1_addr(phys); return bar_rr(o); }
static void reg_wr(u32 phys,u32 v){ u32 o=fixed_map(phys); if(!o)o=l1_addr(phys); bar_wr(o,v); }
static void reg_set(u32 phys,u32 m){ reg_wr(phys,reg_rr(phys)|m); }
static void reg_clr(u32 phys,u32 m){ reg_wr(phys,reg_rr(phys)&~m); }

static int poll_reg(u32 phys,u32 mask,u32 val,u32 ms){
    u64 dl=dkm_rdtsc()+dkm_tsc_per_ms*(u64)ms;
    do { if((reg_rr(phys)&mask)==val) return 0; __asm__ volatile("pause"); } while(dkm_rdtsc()<dl);
    return (reg_rr(phys)&mask)==val?0:-1;
}
static int poll_bar(u32 off,u32 mask,u32 val,u32 ms){
    u64 dl=dkm_rdtsc()+dkm_tsc_per_ms*(u64)ms;
    do { if((bar_rr(off)&mask)==val) return 0; __asm__ volatile("pause"); } while(dkm_rdtsc()<dl);
    return (bar_rr(off)&mask)==val?0:-1;
}

/* ===== PCI ===== */
#define PCI_ADDR_PORT 0xCF8u
#define PCI_DATA_PORT 0xCFCu
static inline void outl_(u16 p,u32 v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline u32 inl_(u16 p){u32 v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p));return v;}
static u32 pci_rd(u8 b,u8 d,u8 f,u8 r){ outl_(PCI_ADDR_PORT,(1u<<31)|((u32)b<<16)|((u32)d<<11)|((u32)f<<8)|(r&0xFC)); return inl_(PCI_DATA_PORT); }
static void pci_wr(u8 b,u8 d,u8 f,u8 r,u32 v){ outl_(PCI_ADDR_PORT,(1u<<31)|((u32)b<<16)|((u32)d<<11)|((u32)f<<8)|(r&0xFC)); outl_(PCI_DATA_PORT,v); }

static int mt_find(void){
    for(u32 b=0;b<256;b++) for(u8 d=0;d<32;d++){
        u32 vd=pci_rd((u8)b,d,0,0);
        if((vd&0xffff)==0xffff) continue;
        if((u16)(vd&0xffff)==MT_VID && (u16)(vd>>16)==MT_DID){ g_bus=(u8)b; g_dev=d; g_func=0; return 0; }
    }
    return -1;
}

/* ===== boot module 固件定位 ===== */
static int fw_find(const char *tag, const u8 **data, u64 *size){
    const struct limine_module_response *rsp=(const struct limine_module_response*)g_api->boot_modules_response;
    if(!rsp) return -1;
    for(u64 i=0;i<rsp->module_count;i++){
        struct limine_file *f=rsp->modules[i];
        if(!f||!f->cmdline) continue;
        const char *a=f->cmdline,*b=tag;
        while(*a&&*a==*b){a++;b++;}
        if(*a==0&&*b==0){ *data=(const u8*)f->address; *size=f->size; return 0; }
    }
    return -1;
}

static u32 be32(const u8 *p){ return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static u32 le32(const u8 *p){ return ((u32)p[3]<<24)|((u32)p[2]<<16)|((u32)p[1]<<8)|p[0]; }
static void memcpy_(void *d,const void *s,u64 n){ u8 *dd=d; const u8 *ss=s; while(n--)*dd++=*ss++; }
static void memset_(void *d,u8 v,u64 n){ u8 *dd=d; while(n--)*dd++=v; }

/* ===== DMA 队列 ===== */
static int q_alloc(struct mt_queue *q,u32 ndesc,u32 ring_base,u32 hw_idx){
    struct dkm_dma_buffer buf;
    u64 bytes=(u64)ndesc*sizeof(struct mt_desc);
    u64 pages=(bytes+4095)/4096;
    if(g_api->dma->alloc_pages(pages,4096,0xFFFFFFFFull,&buf)!=0) return -1;
    memset_(buf.virt,0,pages*4096);
    q->desc=(struct mt_desc*)buf.virt;
    q->desc_phys=buf.phys;
    q->ndesc=ndesc;
    q->head=0;
    q->ring_base=ring_base;
    q->hw_idx=hw_idx;
    return 0;
}
/* 把队列的 desc_base/ring_size 写入硬件 ring 寄存器，并清 cpu/dma idx */
static void q_sync(struct mt_queue *q){
    u32 r=q->ring_base+q->hw_idx*MT_RING_SIZE;
    bar_wr(r+0x00,(u32)q->desc_phys);   /* desc_base */
    bar_wr(r+0x04,q->ndesc);            /* ring_size */
    bar_wr(r+0x08,0);                   /* cpu_idx */
    bar_wr(r+0x0c,0);                   /* dma_idx (hw) */
    q->head=0;
}
static void q_kick(struct mt_queue *q){
    __asm__ volatile("sfence":::"memory");
    bar_wr(q->ring_base+q->hw_idx*MT_RING_SIZE+0x08,q->head);
}

/* 把一段数据丢进 TX 队列（单段），info=0 */
static int tx_raw(struct mt_queue *q,u64 phys,u32 len){
    u32 idx=q->head;
    if((q->head+1)%q->ndesc==0 && q->ndesc<=1) return -1;
    struct mt_desc *d=&q->desc[idx];
    u32 ctrl=((len&0x3fff)<<MT_DMA_CTL_SD_LEN0_SHIFT)|MT_DMA_CTL_LAST_SEC0;
    d->buf0=(u32)phys;
    d->buf1=0;
    d->info=0;
    __asm__ volatile("sfence":::"memory");
    d->ctrl=ctrl;
    q->head=(q->head+1)%q->ndesc;
    q_kick(q);
    return 0;
}

/* 等待 RX MCU ring 出现一个响应，拷到 out（最多 cap），返回长度；超时 -1 */
static int rx_wait(u8 *out,u32 cap,u32 ms){
    u32 r=g_q_rxmcu.ring_base+g_q_rxmcu.hw_idx*MT_RING_SIZE;
    u64 dl=dkm_rdtsc()+dkm_tsc_per_ms*(u64)ms;
    u32 tail=g_q_rxmcu.head;
    do {
        u32 dma_idx=bar_rr(r+0x0c);
        if(dma_idx!=tail){
            struct mt_desc *d=&g_q_rxmcu.desc[tail];
            u32 ctrl=d->ctrl;
            u32 len=(ctrl>>MT_DMA_CTL_SD_LEN0_SHIFT)&0x3fff;
            const u8 *src=(const u8*)(g_api->hhdm_offset + d->buf0);
            u32 n=len<cap?len:cap;
            memcpy_(out,src,n);
            /* 回收：重新武装该 desc（重填 buffer 长度） */
            d->ctrl=((2048u&0x3fff)<<MT_DMA_CTL_SD_LEN0_SHIFT);
            tail=(tail+1)%g_q_rxmcu.ndesc;
            g_q_rxmcu.head=tail;
            bar_wr(r+0x08,tail);  /* cpu_idx 前进，归还 buffer */
            return (int)n;
        }
        __asm__ volatile("pause");
    } while(dkm_rdtsc()<dl);
    return -1;
}

/* 发一条标准 MCU 命令（带 64 字节 txd 头），payload 在 g_tx_scratch 之后；
 * wait=1 时等响应并返回首字节（PATCH_SEM / PATCH_FINISH 用）。 */
static int mcu_send(int cmd_id,int ext_id,const void *payload,u32 plen,int wait,u8 *rbyte){
    u8 *buf=(u8*)g_tx_scratch.virt;
    u32 hdr=64;
    memset_(buf,0,hdr);
    u32 total=hdr+plen;
    if(total>TX_SCRATCH_SZ) return -2;
    memcpy_(buf+hdr,payload,plen);

    u8 seq=++g_mcu_seq & 0xf; if(!seq) seq=++g_mcu_seq & 0xf;
    u32 *txd=(u32*)buf;
    /* txd[0]: TX_BYTES(total) | PKT_FMT(CMD=2)<<23 | Q_IDX(0x20)<<25 */
    txd[0]=(total&0xffff)|(2u<<23)|(0x20u<<25);
    /* txd[1]: LONG_FORMAT bit31 | HDR_FORMAT(CMD=1)<<16 */
    txd[1]=(1u<<31)|(1u<<16);
    /* mcu_txd 覆盖在 offset 32 */
    u8 *m=buf+32;
    u16 mlen=(u16)(total-32);
    m[0]=(u8)(mlen&0xff); m[1]=(u8)(mlen>>8);           /* len */
    u16 pq=(u16)((1u<<15)|(0x20u<<10));
    m[2]=(u8)(pq&0xff); m[3]=(u8)(pq>>8);                /* pq_id */
    m[4]=(u8)cmd_id;                                     /* cid */
    m[5]=MCU_PKT_ID;                                     /* pkt_type */
    m[6]=ext_id?2:0;                                     /* set_query: SET=2 for ext */
    m[7]=seq;                                            /* seq */
    m[9]=(u8)ext_id;                                     /* ext_cid */
    m[10]=0;                                             /* s2d_index=H2N */
    m[11]=ext_id?1:0;                                    /* ext_cid_ack */

    if(tx_raw(&g_q_wm,g_tx_scratch.phys,total)!=0) return -3;

    if(!wait) { dkm_delay_ms(2); return 0; }
    static u8 rbuf[2048];
    int n=rx_wait(rbuf,sizeof(rbuf),1000);
    if(n<0) return -4;
    /* rxd 头 36 字节；seq 在 offset 33（rxd[6]=24, len+pkt=4 →28, eid=28,seq=29） */
    if(n>=34){
        u8 rseq=rbuf[29];
        if(rseq!=seq) { /* 不匹配也不致命，返回 0 继续 */ }
        if(rbyte) *rbyte=rbuf[32];   /* PATCH_SEM/FINISH: sizeof(rxd)-4 = 32 */
    }
    return 0;
}

/* FW_SCATTER：无头，走 FWDL 队列，分片 4096 */
static int fw_scatter(const u8 *data,u32 len){
    u8 *scr=(u8*)g_tx_scratch.virt;
    while(len>0){
        u32 cur=len<FWDL_MAX_LEN?len:FWDL_MAX_LEN;
        memcpy_(scr,data,cur);
        __asm__ volatile("sfence":::"memory");
        if(tx_raw(&g_q_fwdl,g_tx_scratch.phys,cur)!=0) return -1;
        /* 等 fwdl dma_idx 追上（简化：轮询 dma_idx==head） */
        u32 r=g_q_fwdl.ring_base+g_q_fwdl.hw_idx*MT_RING_SIZE;
        u64 dl=dkm_rdtsc()+dkm_tsc_per_ms*200;
        while(bar_rr(r+0x0c)!=g_q_fwdl.head && dkm_rdtsc()<dl) __asm__ volatile("pause");
        data+=cur; len-=cur;
    }
    return 0;
}

static int mcu_init_download(u32 addr,u32 len,u32 mode){
    struct { u32 addr,len,mode; } req={addr,len,mode};
    int cmd = (addr==0x900000)?MCU_CMD_PATCH_START_REQ:MCU_CMD_TARGET_ADDRESS_LEN_REQ;
    return mcu_send(cmd,0,&req,sizeof(req),1,0);
}

/* ===== driver own / wfsys reset / dma ===== */
static int drv_own(void){
    for(int i=0;i<10;i++){
        reg_wr(MT_CONN_ON_LPCTL,PCIE_LPCR_HOST_CLR_OWN);
        if(poll_reg(MT_CONN_ON_LPCTL,PCIE_LPCR_HOST_OWN_SYNC,0,50)==0) return 0;
    }
    return -1;
}
static int fw_own(void){
    for(int i=0;i<10;i++){
        reg_wr(MT_CONN_ON_LPCTL,PCIE_LPCR_HOST_SET_OWN);
        if(poll_reg(MT_CONN_ON_LPCTL,PCIE_LPCR_HOST_OWN_SYNC,PCIE_LPCR_HOST_OWN_SYNC,50)==0) return 0;
    }
    return -1;
}
static int top_drv_own(void){
    reg_wr(MT_TOP_LPCR_HOST_BAND0,MT_TOP_LPCR_HOST_DRV_OWN);
    return poll_reg(MT_TOP_LPCR_HOST_BAND0,MT_TOP_LPCR_HOST_FW_OWN,0,500);
}
static int wfsys_reset(void){
    reg_clr(MT_WFSYS_RST,WFSYS_SW_RST_B);
    dkm_delay_ms(50);
    reg_set(MT_WFSYS_RST,WFSYS_SW_RST_B);
    return poll_reg(MT_WFSYS_RST,WFSYS_SW_INIT_DONE,WFSYS_SW_INIT_DONE,500);
}

static int dma_disable(int force){
    bar_clr(MT_WFDMA0_GLO_CFG,
        MT_WFDMA0_GLO_CFG_TX_DMA_EN|MT_WFDMA0_GLO_CFG_RX_DMA_EN|
        MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN|
        MT_WFDMA0_GLO_CFG_OMIT_TX_INFO|MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);
    if(poll_bar(MT_WFDMA0_GLO_CFG,MT_WFDMA0_GLO_CFG_TX_DMA_BUSY|MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,0,100)!=0)
        return -1;
    bar_clr(MT_WFDMA0_GLO_CFG_EXT0,MT_WFDMA0_GLO_CFG_EXT0_CSR_TX_DMASHDL_EN);
    reg_set(MT_DMASHDL_SW_CONTROL,MT_DMASHDL_DMASHDL_BYPASS);
    if(force){
        bar_clr(MT_WFDMA0_RST,MT_WFDMA0_RST_DMASHDL_ALL_RST|MT_WFDMA0_RST_LOGIC_RST);
        bar_set(MT_WFDMA0_RST,MT_WFDMA0_RST_DMASHDL_ALL_RST|MT_WFDMA0_RST_LOGIC_RST);
    }
    return 0;
}
static int dma_enable(void){
    bar_wr(MT_WFDMA0_RST_DTX_PTR,~0u);
    bar_wr(MT_WFDMA0_PRI_DLY_INT_CFG0,0);
    bar_set(MT_WFDMA0_GLO_CFG,
        MT_WFDMA0_GLO_CFG_TX_WB_DDONE|MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN|
        MT_WFDMA0_GLO_CFG_CLK_GAT_DIS|MT_WFDMA0_GLO_CFG_OMIT_TX_INFO|
        (3u<<MT_WFDMA0_GLO_CFG_DMA_SIZE_SHIFT)|MT_WFDMA0_GLO_CFG_RX_WB_DDONE|
        MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN|MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);
    bar_set(MT_WFDMA0_GLO_CFG,MT_WFDMA0_GLO_CFG_TX_DMA_EN|MT_WFDMA0_GLO_CFG_RX_DMA_EN);
    bar_set(MT_WFDMA_DUMMY_CR,MT_WFDMA_NEED_REINIT);
    return 0;
}

/* RX MCU ring 填充 buffer */
static int rx_fill(void){
    u64 pages=(RX_NDESC*2048+4095)/4096;
    if(g_api->dma->alloc_pages(pages,4096,0xFFFFFFFFull,&g_rx_bufs)!=0) return -1;
    memset_(g_rx_bufs.virt,0,pages*4096);
    for(u32 i=0;i<RX_NDESC;i++){
        struct mt_desc *d=&g_q_rxmcu.desc[i];
        u64 bp=g_rx_bufs.phys+(u64)i*2048;
        d->buf0=(u32)bp;
        d->buf1=0;
        d->info=0;
        d->ctrl=((2048u&0x3fff)<<MT_DMA_CTL_SD_LEN0_SHIFT);
    }
    __asm__ volatile("sfence":::"memory");
    /* cpu_idx 指到最后，表示全部 buffer 可用 */
    u32 r=g_q_rxmcu.ring_base+g_q_rxmcu.hw_idx*MT_RING_SIZE;
    bar_wr(r+0x08,RX_NDESC-1);
    g_q_rxmcu.head=0;
    return 0;
}

static int dma_init(void){
    if(dma_disable(1)!=0){ rlog("[mt7920] dma_disable timeout\n"); }
    if(q_alloc(&g_q_wm,256,MT_TX_RING_BASE,TXQ_MCU_WM)!=0) return -1;
    if(q_alloc(&g_q_fwdl,128,MT_TX_RING_BASE,TXQ_FWDL)!=0) return -1;
    if(q_alloc(&g_q_rxmcu,RX_NDESC,MT_RX_EVENT_RING_BASE,RXQ_MCU_WM)!=0) return -1;
    q_sync(&g_q_wm);
    q_sync(&g_q_fwdl);
    q_sync(&g_q_rxmcu);
    if(rx_fill()!=0) return -1;
    dma_enable();
    return 0;
}

/* ===== 固件下载 ===== */
static int load_patch(const u8 *fw,u64 fwsz){
    u8 sem=0;
    struct { u32 op; } semreq;
    semreq.op=PATCH_SEM_GET;
    mcu_send(MCU_CMD_PATCH_SEM_CONTROL,0,&semreq,sizeof(semreq),1,&sem);
    rhex("[mt7920] patch sem=",sem);
    if(sem==PATCH_IS_DL) return 0;
    if(sem!=PATCH_NOT_DL_SEM_SUCCESS){ rlog("[mt7920] patch sem fail\n"); }

    if(fwsz<sizeof(struct patch_hdr)){ mt_panic("MT7920-E20 PATCH FW TOO SMALL"); return -1; }
    const struct patch_hdr *h=(const struct patch_hdr*)fw;
    u32 nreg=be32((const u8*)&h->desc.n_region);
    rhex("[mt7920] patch regions=",nreg);

    int ret=0;
    for(u32 i=0;i<nreg;i++){
        const struct patch_sec *s=(const struct patch_sec*)(fw+sizeof(struct patch_hdr)+i*sizeof(struct patch_sec));
        if((be32((const u8*)&s->type)&PATCH_SEC_TYPE_MASK)!=PATCH_SEC_TYPE_INFO){ mt_panic("MT7920-E21 PATCH SEC TYPE"); ret=-1; break; }
        u32 addr=be32((const u8*)&s->addr);
        u32 len=be32((const u8*)&s->len);
        u32 offs=be32((const u8*)&s->offs);
        u32 mode=DL_MODE_NEED_RSP;  /* plain，MT7920 patch 非加密段 */
        rhex("[mt7920] patch addr=",addr);
        rhex("[mt7920] patch len=",len);
        if(mcu_init_download(addr,len,mode)!=0){ rlog("[mt7920] init_download fail\n"); ret=-1; break; }
        if(fw_scatter(fw+offs,len)!=0){ rlog("[mt7920] scatter fail\n"); ret=-1; break; }
    }
    /* PATCH_FINISH */
    struct { u8 check_crc; u8 rsv[3]; } fin; memset_(&fin,0,sizeof(fin));
    mcu_send(MCU_CMD_PATCH_FINISH_REQ,0,&fin,sizeof(fin),1,0);

    /* release sem */
    semreq.op=PATCH_SEM_RELEASE;
    mcu_send(MCU_CMD_PATCH_SEM_CONTROL,0,&semreq,sizeof(semreq),1,&sem);
    return ret;
}

static int load_ram(const u8 *fw,u64 fwsz){
    if(fwsz<sizeof(struct fw_trailer)){ mt_panic("MT7920-E22 RAM FW TOO SMALL"); return -1; }
    const struct fw_trailer *tr=(const struct fw_trailer*)(fw+fwsz-sizeof(struct fw_trailer));
    u8 nreg=tr->n_region;
    rhex("[mt7920] ram regions=",nreg);
    u32 offset=0, override=0, option=0;
    for(u8 i=0;i<nreg;i++){
        const struct fw_region *rg=(const struct fw_region*)((const u8*)tr-((u32)(nreg-i))*sizeof(struct fw_region));
        u32 len=le32((const u8*)&rg->len);
        u32 addr=le32((const u8*)&rg->addr);
        u32 mode=DL_MODE_NEED_RSP;
        if(rg->feature_set&FW_FEATURE_NON_DL){ offset+=len; continue; }
        if(rg->feature_set&FW_FEATURE_OVERRIDE_ADDR) override=addr;
        rhex("[mt7920] ram addr=",addr);
        rhex("[mt7920] ram len=",len);
        if(mcu_init_download(addr,len,mode)!=0){ rlog("[mt7920] ram init_download fail\n"); return -1; }
        if(fw_scatter(fw+offset,len)!=0){ rlog("[mt7920] ram scatter fail\n"); return -1; }
        offset+=len;
    }
    if(override) option|=FW_START_OVERRIDE;
    struct { u32 option,addr; } sreq={option,override};
    mcu_send(MCU_CMD_FW_START_REQ,0,&sreq,sizeof(sreq),1,0);
    return 0;
}

static int load_firmware(void){
    const u8 *patch,*ram; u64 psz,rsz;
    if(fw_find("fw:mt7920_patch",&patch,&psz)!=0){ mt_panic("MT7920-E23 PATCH FW MODULE MISSING"); return -1; }
    if(fw_find("fw:mt7920_ram",&ram,&rsz)!=0){ mt_panic("MT7920-E24 RAM FW MODULE MISSING"); return -1; }
    rhex("[mt7920] patch fw size=",psz);
    rhex("[mt7920] ram fw size=",rsz);

    /* NIC_POWER_CTRL restart + 等 FW_PWR_ON */
    struct { u8 pm; u8 rsv[3]; } pr; memset_(&pr,0,sizeof(pr)); pr.pm=1;
    mcu_send(MCU_CMD_NIC_POWER_CTRL,0,&pr,sizeof(pr),0,0);
    if(poll_reg(MT_CONN_ON_MISC,MT_TOP_MISC_FW_STATE,MT_TOP_MISC2_FW_PWR_ON,1000)!=0)
        rlog("[mt7920] WARN MCU not ready for fw dl\n");

    if(load_patch(patch,psz)!=0){ mt_panic("MT7920-E25 PATCH DOWNLOAD FAILED"); return -1; }
    rlog("[mt7920] patch downloaded\n");
    if(load_ram(ram,rsz)!=0){ mt_panic("MT7920-E26 RAM DOWNLOAD FAILED"); return -1; }
    rlog("[mt7920] ram downloaded\n");

    if(poll_reg(MT_CONN_ON_MISC,MT_TOP_MISC2_FW_N9_RDY,MT_TOP_MISC2_FW_N9_RDY,1500)!=0){
        mt_panic("MT7920-E27 FIRMWARE N9 NOT READY");
        return -1;
    }
    rlog("[mt7920] N9 firmware ready\n");
    return 0;
}

/* ===== netdev 回调（扫描/关联未实现，显式 panic） ===== */
static int mt_scan_start(void *c){(void)c;mt_panic("MT7920-E10 WIFI SCAN NOT IMPLEMENTED");return -1;}
static int mt_scan_count(void *c){(void)c;mt_panic("MT7920-E10 WIFI SCAN NOT IMPLEMENTED");return 0;}
static int mt_scan_result(void *c,u32 n,struct dkm_net_scan_result *o){(void)c;(void)n;(void)o;mt_panic("MT7920-E10 WIFI SCAN NOT IMPLEMENTED");return -1;}
static int mt_connect(void *c,const char *s,const char *p){(void)c;(void)s;(void)p;mt_panic("MT7920-E11 WLAN CONNECT NOT IMPLEMENTED");return -1;}
static int mt_disconnect(void *c){(void)c;mt_panic("MT7920-E12 WLAN DISCONNECT NOT IMPLEMENTED");return -1;}
static int mt_is_wireless(void *c){(void)c;return 1;}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,struct dkm_driver_handle *handle){
    (void)handle;
    if(!api||!api->log) return -1;
    dkm_tsc_calibrate();
    g_api=api; g_log=(struct dkm_log_api*)api->log;
    dkm_instr_init(api);
    rlog("[mt7920] driver_init entered\n");

    if(mt_find()!=0){ rlog("[mt7920] 14c3:7920 not found\n"); return 0; }
    rhex("[mt7920] b:d:f=",((u64)g_bus<<16)|((u64)g_dev<<8)|g_func);

    u32 cmd=pci_rd(g_bus,g_dev,g_func,0x04);
    cmd|=(1u<<1)|(1u<<2);
    pci_wr(g_bus,g_dev,g_func,0x04,cmd);

    u32 bar0=pci_rd(g_bus,g_dev,g_func,0x10);
    if(bar0&1u){ mt_panic("MT7920-E02 BAR0 IS IO"); return -1; }
    u64 phys=(u64)(bar0&0xFFFFFFF0u);
    if((bar0&0x6u)==0x4u) phys|=((u64)pci_rd(g_bus,g_dev,g_func,0x14)<<32);
    rhex("[mt7920] BAR0 phys=",phys);
    if(!phys){ mt_panic("MT7920-E02 BAR0 MISSING"); return -1; }
    if(!api->mm_map_mmio){ mt_panic("MT7920-E02 MMIO MAP API MISSING"); return -1; }
    void *va=api->mm_map_mmio(phys,0x200000);  /* 2MB BAR0 窗口 */
    if(!va){ mt_panic("MT7920-E02 MMIO MAP FAILED"); return -1; }
    g_bar=(volatile u8*)va;

    /* 先让固件 own，再取 driver own（对拍 probe 顺序） */
    fw_own();
    if(drv_own()!=0){ mt_panic("MT7920-E03 DRIVER OWN FAILED"); g_bar=0; return -1; }
    rlog("[mt7920] driver own ok\n");

    u32 chip=reg_rr(MT_HW_CHIPID);
    u32 rev=reg_rr(MT_HW_REV);
    rhex("[mt7920] chipid=",chip);
    rhex("[mt7920] rev=",rev);

    if(wfsys_reset()!=0){ rlog("[mt7920] WARN wfsys reset timeout\n"); }
    else rlog("[mt7920] wfsys reset done\n");

    bar_wr(MT_WFDMA0_HOST_INT_ENA,0);
    bar_wr(MT_PCIE_MAC_INT_ENABLE,0xff);

    if(dma_init()!=0){ mt_panic("MT7920-E04 DMA INIT FAILED"); g_bar=0; return -1; }
    rlog("[mt7920] dma init ok\n");

    /* TX scratch */
    if(api->dma->alloc_pages((TX_SCRATCH_SZ+4095)/4096,4096,0xFFFFFFFFull,&g_tx_scratch)!=0){
        mt_panic("MT7920-E05 TX SCRATCH ALLOC FAILED");
        g_bar=0; return -1;
    }

    /* MCU own via TOP LPCR，再下载固件 */
    if(top_drv_own()!=0){ rlog("[mt7920] WARN top drv own timeout\n"); }
    if(load_firmware()!=0){ mt_panic("MT7920-E06 FIRMWARE LOAD FAILED"); g_bar=0; return -1; }

    /* 注册 netdev（link 需后续 phy/连接阶段才 up） */
    if(api->net&&api->net->register_device){
        struct dkm_net_device_desc nd; memset_(&nd,0,sizeof(nd));
        nd.name="wlan0";
        nd.flags=(1u<<3);
        nd.scan_start=mt_scan_start;
        nd.scan_count=mt_scan_count;
        nd.scan_result=mt_scan_result;
        nd.is_wireless=mt_is_wireless;
        nd.connect=mt_connect;
        nd.disconnect=mt_disconnect;
        int idx=api->net->register_device(&nd);
        rhex("[mt7920] netdev index=",(u64)(i32)idx);
    }
    rlog("[mt7920] init done (firmware running; scan/connect pending)\n");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle){ (void)handle; g_bar=0; return 0; }
