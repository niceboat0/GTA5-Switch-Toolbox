/* Included by ui_adapter.h after its shared dialog/row helpers. */
#include "gc_package.h"
static GcPackage g_radio_packs[GC_PACKAGE_MAX];
static int g_radio_count, g_radio_state[GC_PACKAGE_MAX];
static char g_radio_status[512];
static int g_radio_ok;
static const char *modern_radio_title(const GcPackage *p) {
    return !g_lang_en && p->title_zh[0]?p->title_zh:p->title;
}
static void modern_radio_scan(int section) {
    g_radio_count=gc_package_scan(g_radio_packs,GC_PACKAGE_MAX,section==0?"selfradio":"radio");
    if(g_radio_count<0) g_radio_count=0;
    for(int i=0;i<GC_PACKAGE_MAX;i++) g_radio_state[i]=-1;
    snprintf(g_radio_status,sizeof(g_radio_status),TR("找到 %d 个安装包；可选择「检查状态」。","Found %d packages; choose Check status to verify installation."),g_radio_count);
    g_radio_ok=1;
}
static void modern_radio_help(void) {
    modern_report(TR("电台使用说明","Radio setup"),TR(
        "自定义电台（Self Radio NX v1.1）\n"
        "运行包未内置。作者下载目录目前为空，需另行取得匹配的 HARD 运行包和 release-verification.json。\n"
        "用附带的 export_package.py selfradio 准备安装包，将整个输出文件夹复制到：\n"
        "sdmc:/switch/gta5save/packages/<包名>/package.txt\n"
        "音乐放到游戏 romfs/User Music/。不能直接把 MP3 当作安装包。\n"
        "v1.1 配套构建：6e3e9b3af5746e74b615bd981080c9271bc226620。安装前核对游戏构建和运行包说明。\n\n"
        "替换原电台\n"
        "电脑端 GTA Radio Editor 生成 RADIO_*.rpf 与 audio_rel.rpf 后，用 export_package.py radio 成对打包。工具不转换音乐。\n\n"
        "安装与恢复\n"
        "准备完整 update.rpf 备份和足够空间（临时文件与备份可能各占一份完整 RPF）。安装检查来源哈希及目标原文，拒绝未知改动；备份在 packages/.backups/。\n"
        "还原需本工具保留的备份，后续改动可能阻止还原。断电后先运行「中断恢复」。完成后重启游戏。尚待实机测试。",
        "Self Radio NX v1.1\n"
        "Runtime not bundled. The author's download folder is currently empty. Obtain the matching HARD runtime and release-verification.json separately.\n"
        "Prepare it with export_package.py selfradio, then copy the entire output folder to:\n"
        "sdmc:/switch/gta5save/packages/<package>/package.txt\n"
        "Music belongs in the game's romfs/User Music/ folder. MP3 files are not installation packages.\n"
        "v1.1 matching build: 6e3e9b3af5746e74b615bd981080c9271bc226620. Check the game build and runtime instructions first.\n\n"
        "Replace a station\n"
        "Create RADIO_*.rpf and audio_rel.rpf using desktop GTA Radio Editor. Export them together with export_package.py radio. This tool does not encode music.\n\n"
        "Installation and recovery\n"
        "Keep a complete update.rpf backup and enough space for staging and backups (each may need a full RPF copy). Source hashes and baseline content are checked; unknown edits are refused. Backups: packages/.backups/.\n"
        "Restoration requires retained backups; later modifications can block it. After interruption run Recover interrupted first. Restart the game afterwards. Console testing is pending."));
}
static void modern_radio_build(void) {
    g_view.title=TR("电台","Radio");g_view.subtitle=TR("安装自定义电台运行包，或导入电脑生成的成对电台文件。","Install a Self Radio runtime or import paired station files prepared on a PC.");
    g_view.section_count=3;g_view.sections[0]=TR("自定义电台","Self Radio");g_view.sections[1]=TR("替换原电台","Replace station");g_view.sections[2]=TR("使用说明","Setup guide");
    g_view.primary=TR("A  查看安装确认","A  Review installation");
    if(g_ui.section==2) {
        modern_row(TR("安装包与音乐目录","Packages & music folders"),TR("安装条件、准备方法和恢复流程","Runtime requirements, package export and restoration"),TR("说明","Guide"),TR("未内置运行包；安装入口不代表已经启用电台。","Runtime not bundled; opening this page does not enable Self Radio."),U_RADIO_HELP,0,UI_WARN,0);
        modern_button(TR("查看说明","Read guide"),U_RADIO_HELP,0,0,0);
        modern_button(TR("中断恢复","Recover interrupted"),U_RADIO_RECOVER,0,0,0);
    } else {
        for(int i=0;i<g_radio_count;i++) {
            const GcPackage *p=&g_radio_packs[i];char sub[192];
            snprintf(sub,sizeof(sub),TR("%s  /  %d 个文件","%s  /  %d files"),p->id,p->count);
            modern_row(modern_radio_title(p),sub,g_radio_state[i]<0?TR("未检查","Unchecked"):g_radio_state[i]?TR("已安装","Installed"):TR("未安装 / 不一致","Absent / mixed"),TR("安装将校验并备份目标文件。请核对配套游戏构建，保留完整 RPF 备份。尚待实机测试。","Installation validates and backs up target files. Check the matching game build and keep a full RPF backup. Console testing is pending."),U_RADIO_INSTALL,i,UI_WARN,0);
        }
        modern_button(TR("安装所选","Install selected"),UI_RUN_ROW,0,!g_radio_count,0);
        modern_button(TR("还原备份","Restore backup"),U_RADIO_RESTORE,0,!g_radio_count,1);
        modern_button(TR("检查状态","Check status"),U_RADIO_CHECK,0,!g_radio_count,0);
        modern_button(TR("重新扫描","Rescan"),U_RADIO_SCAN,0,0,0);
        modern_button(TR("中断恢复","Recover interrupted"),U_RADIO_RECOVER,0,0,0);
        modern_button(TR("使用说明","Setup guide"),U_RADIO_HELP,0,0,0);
    }
    g_view.empty_title=TR("没有找到电台安装包","No radio packages found");
    g_view.empty_body=TR("运行包未内置。先阅读「使用说明」，准备包含 package.txt 的安装包文件夹，再重新扫描。","Runtime not bundled. Read Setup guide, prepare a package folder containing package.txt, then rescan.");
    g_view.empty_path=GC_PACKAGE_HOME;g_view.status=g_radio_status;g_view.status_tone=g_radio_ok?UI_MUTED:UI_ERROR;
}
static void modern_radio_progress(unsigned long long done,unsigned long long total,void *user) {
    (void)user;dlc_prog_set(NULL,NULL,total?(int)((double)done*100.0/(double)total):-1);dlc_prog_frame();
}
/* Package commits rename archives. Invalidate every shared archive/XML cache. */
static void modern_radio_close_archives(void) {
    if(g_gc_opened) {gc_rpf_close(&g_gc_rpf);g_gc_opened=0;}
    free(g_gc_xml);g_gc_xml=NULL;g_gc_xml_len=0;gc_clear_edited();
    free(g_dlc_xml);g_dlc_xml=NULL;g_dlc_xml_len=0;g_dlc_xml_loaded=0;
    free(g_dlc_etud);g_dlc_etud=NULL;g_dlc_etud_len=0;g_dlc_etud_loaded=0;
}
static void modern_radio_execute(UiEvent e,int confirmed) {
    if(e.action==U_RADIO_HELP) {modern_radio_help();return;}
    if(e.action==U_RADIO_SCAN) {modern_radio_scan(g_ui.section);return;}
    int i=e.action==U_RADIO_INSTALL?e.param:g_ui.selected;
    if(e.action!=U_RADIO_RECOVER && (i<0 || i>=g_radio_count)) return;
    const GcPackage *p=e.action==U_RADIO_RECOVER?NULL:&g_radio_packs[i];
    if(e.action==U_RADIO_CHECK) {
        dlc_prog_begin(TR("正在检查电台状态","Checking radio status"),modern_radio_title(p));dlc_prog_frame();
        g_radio_state[i]=gc_package_state(p);dlc_prog_end();
        modern_report(TR("电台安装状态","Radio status"),g_radio_state[i]?TR("全部目标文件与安装包匹配。","All target files match the package."):TR("未安装，或部分目标与安装包不一致。","Absent or some target content differs from the package."));return;
    }
    if(g_gc_dirty || g_dlc_dirty) {modern_report(TR("请先保存参数修改","Save parameter changes first"),TR("update.rpf 还有未写入修改。先保存参数或 DLC 修改，再操作电台。","update.rpf has pending edits. Save parameters or DLC changes before changing radio files."));return;}
    const char *operation=e.action==U_RADIO_INSTALL?TR("安装电台","Install radio"):e.action==U_RADIO_RESTORE?TR("还原电台备份","Restore radio backup"):TR("恢复中断操作","Recover interrupted operation");
    if(!confirmed) {
        char body[2048];snprintf(body,sizeof(body),TR("%s\n%s\n\n目标：%s\n\n将校验并写入游戏文件，保留备份与恢复记录。需要足够临时空间，请先保留完整 RPF 备份。操作完成前请勿退出。尚待实机测试。","%s\n%s\n\nTarget: %s\n\nValidates and writes game files, retaining backups and recovery records. Allow enough staging space and keep a full RPF backup. Keep the tool open until completion. Console testing is pending."),operation,p?modern_radio_title(p):TR("处理中断日志","Process interruption journal"),GC_PACKAGE_GAME);
        modern_dialog(1,operation,body,e);return;
    }
    modern_radio_close_archives();dlc_prog_begin(operation,p?modern_radio_title(p):"");dlc_prog_frame();
    char err[2048]={0};int rc;
    if(e.action==U_RADIO_INSTALL) rc=gc_package_install(p,modern_radio_progress,NULL,err,sizeof(err));
    else if(e.action==U_RADIO_RESTORE) rc=gc_package_restore_progress(p,modern_radio_progress,NULL,err,sizeof(err));
    else rc=gc_package_recover_progress(modern_radio_progress,NULL,err,sizeof(err));
    dlc_prog_end();for(int k=0;k<g_radio_count;k++) g_radio_state[k]=-1;g_radio_ok=rc==0;
    snprintf(g_radio_status,sizeof(g_radio_status),"%s",err[0]?err:rc?TR("操作失败","Operation failed"):TR("操作完成；重启游戏生效。","Complete; restart the game."));
    modern_report(rc?TR("电台操作失败","Radio operation failed"):TR("电台操作完成","Radio operation complete"),err[0]?err:g_radio_status);
}
