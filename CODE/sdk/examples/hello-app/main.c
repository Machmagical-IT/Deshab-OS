/* hello-app - Deshab SDK 最小应用示例
 *
 * 展示完整的应用入口契约：校验 magic -> 初始化上下文 -> 绘制 UI -> 等待退出。
 * 构建产物：SYSTEM/system/deshab64/tools/hello.elf
 */
#include "../../../firstInit/ascii_bitmaps.c"
#include "deshab/deshab.h"
#include "deshab/font.h"

static dsb_app_context g_ac;

void dsk_entry(const dsk_boot_context *ctx) {
    /* 1. 校验启动上下文 magic */
    if (ctx->magic != DSK_BOOT_MAGIC) return;

    /* 2. 初始化应用上下文（framebuffer + block + kernel_api） */
    dsb_app_init(&g_ac, ctx);

    /* 3. 清屏 */
    dsb_fill_bg(&g_ac, DSB_BG_PRIMARY);

    /* 4. 标题栏 + 状态栏 */
    dsb_draw_titlebar(&g_ac, "HelloApp", (i64)g_ac.fb_w);
    dsb_draw_statusbar(&g_ac, "Esc to exit", (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    /* 5. 居中显示欢迎文字 */
    const char *msg = "Hello, Deshab!";
    i64 msg_w = (i64)dsb_strlen(msg) * 12;
    i64 x = ((i64)g_ac.fb_w - msg_w) / 2;
    i64 y = ((i64)g_ac.fb_h) / 2;
    dsb_draw_string(&g_ac, msg, x, y, DSB_TEXT_PRIMARY, DSB_BG_PRIMARY, 12);

    /* 6. 等待 Esc 键退出
     *
     * P6b 之后 ps2kbd.drv 常驻，IRQ1 handler 会把端口 0x60 的字节读走；
     * 这里若还直接轮询 0x60 就会饿死（按 Esc 没反应）。改走服务总线
     * srv.input.kbd.poll，与 diskinfo/sysinfo 等工具同款。 */
    static dsb_input g_in;
    if (dsb_input_open(&g_in, g_ac.api) != 0) {
        /* kbd 服务缺席 = 无法交互；画提示后退出（零降级：不碰端口）。 */
        dsb_draw_string(&g_ac, "srv.input.kbd.poll missing", 12, 60,
                        DSB_ERROR, DSB_BG_PRIMARY, 12);
        return;
    }
    for (;;) {
        u8 sc;
        if (dsb_input_kbd(&g_in, &sc) && sc == 1) break;  /* Esc make code */
        __asm__ volatile("pause");
    }
}
