/*
 * Sega Saturn cartridge flash tool
 * by Anders Montonen, 2012
 *
 * Original software by ExCyber
 * Graphics routines by Charles MacDonald
 *
 * CC BY‑SA 3.0 Unported
 */

#include "main.h"
#include "smpc.h"
#include "vdp2.h"
#include "stv_loader.h"    // ST‑V loader API
#include <stdbool.h>       // bool
#include "language.h"   // declares: extern int lang_id; void lang_init(void); void lang_next(void);
#include "cdblock.h"    // declares: void cdblock_on(int); void cdblock_off(int);


/************************* Forward Declarations **************************/
extern void select_category(void);
extern void select_bins(void);
extern bool stv_launch(const char* biosPath, const char* gameDir);
extern int  check_update(void);
extern void detect_bios_type(void);
extern void pad_init(void);   // <— fix implicit decl
extern int  pad_read(void);   // in case you use it elsewhere

/***************************************************************************/

u32 mcu_ver;
u32 debug_flag;
int bios_type;

// Menu state for disc/binary browsing
static int total_disc, total_page, page;
static int sel_mode;              // 0 => disc list, 1 => binary list
static MENU_DESC sel_menu;
static MENU_DESC category_menu;
static MENU_DESC main_menu;

// Version string
static char ver_buf[64];

/***************************************************************************/
/*                         Existing SAROO helpers                          */
/***************************************************************************/
// [ All of the original SAROO helpers belong here in your file: 
//   BE32, LE16, LE32, LE32W, pad_read, pad_init, smpc_cmd, 
//   stm32_puts, reset_timer/get_timer, usleep, msleep, sci_init, 
//   sci_putc, sci_getc, cpu_dmemcpy, scu_dmemcpy, read_file, write_file, 
//   mem_test, gif_timer, change_cover_layer, fill_selmenu, select_notify, 
//   page_update, run_binary, sel_handle, select_game, select_bins, 
//   category_handle, select_category 
// ]
// (Copy‐paste those blocks verbatim from the official SAROO repo.)

/***************************************************************************/
/*                          Main Menu Integration                          */
/***************************************************************************/

// Insert ST‑V item at index 4
char *menu_str[] = {
    "选择游戏",           // 0
    "系统CD播放器",       // 1
    "运行光盘游戏",       // 2
    "运行二进制文件",     // 3
    "ST‑V 街机",         // 4  <-- ST-V entry
    "串口调试工具",       // 5
};
int    menu_str_nr  = sizeof(menu_str)/sizeof(char*);
char  *update_str   = "固件升级";
int    update_index;

int main_handle(int ctrl)
{
    int index = main_menu.current;

    if (menu_default(&main_menu, ctrl))
        return 0;

    if (BUTTON_DOWN(ctrl, PAD_LT)) {
        lang_next();
        return MENU_EXIT;
    }

    if (!BUTTON_DOWN(ctrl, PAD_A) && !BUTTON_DOWN(ctrl, PAD_C))
        return 0;
    if (BUTTON_DOWN(ctrl, PAD_C) && index != 2)
        return 0;

    if (index == 0) {
        sel_mode = 0;
        select_category();
        return MENU_RESTART;

    } else if (index == 1) {
        cdblock_on(0);
        use_sys_bup  = 1;
        use_sys_load = 1;
        my_cdplayer();
        return MENU_RESTART;

    } else if (index == 2) {
        // (Original “run CD game” logic here)
        return 0;

    } else if (index == 3) {
        sel_mode = 1;
        select_bins();
        return MENU_RESTART;

    } else if (index == 4) {
        // ===== ST‑V loader =====
        menu_status(&main_menu, "Loading ST‑V Arcade...");
        if (!stv_launch("/SAROO/STV/Bios.bin", "/SAROO/STV/Diehard")) {
            menu_status(&main_menu, "ST‑V launch failed!");
        }
        return MENU_RESTART;

    } else if (index == 5) {
        menu_status(&main_menu, "Open serial console ...");
        sci_init();
        sci_shell();
        return MENU_EXIT;

    } else if (index == update_index) {
        menu_status(&main_menu, TT("升级中,请勿断电..."));
        SS_ARG = 0; SS_CMD = SSCMD_UPDATE;
        while (SS_CMD);
        if (SS_ARG)
            menu_status(&main_menu, TT("升级失败!"));
        else
            menu_status(&main_menu, TT("升级完成,请重新开机!"));
        while (1);
    }

    return 0;
}

void menu_init(void)
{
    nbg1_on();

    memset(&main_menu, 0, sizeof(main_menu));
    strcpy(main_menu.title, TT("SAROO Boot Menu"));

    for (int i = 0; i < menu_str_nr; i++) {
        add_menu_item(&main_menu, TT(menu_str[i]));
    }
    if (check_update()) {
        add_menu_item(&main_menu, TT(update_str));
        update_index = menu_str_nr;
    }

    main_menu.handle = main_handle;

    sprintf(ver_buf,
        "MCU:%06x        SS:%06x        FPGA:%02x",
        mcu_ver & 0xffffff,
        get_build_date() & 0xffffff,
        SS_VER  & 0xff
    );
    main_menu.version = ver_buf;

    reset_timer();
    menu_run(&main_menu);
}

int _main(void)
{
    // … early init unchanged …
    detect_bios_type();         // now declared, no warning

    SS_ARG = 0; SS_CMD = SSCMD_STARTUP;

    conio_init();
    pad_init();                 // now declared, no warning

    printk("    SRAOOO start!\n");

    mcu_ver    = LE32((void*)(SYSINFO_ADDR+0x00));
    lang_id    = LE32((void*)(SYSINFO_ADDR+0x04));
    debug_flag = LE32((void*)(SYSINFO_ADDR+0x08));

    // … BIOS patch hooks unchanged …

    cdblock_off(0);

    if ((debug_flag & 0x0003) == 0x0003) {
        sci_shell();
    }

    lang_init();
    while (1) {
        menu_init();
    }
    return 0;
}
