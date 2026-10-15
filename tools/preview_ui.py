"""Render the actual C presentation module with sample data and a local font.

cc -shared -fPIC source/ui.c -o build/ui-preview.so
python tools/preview_ui.py --library build/ui-preview.so --out build/previews

On Windows use a C compiler to produce build/ui-preview.dll instead.
Requires Pillow. These are preview fixtures, not a live Switch device capture.
"""
import argparse
import ctypes as c
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

class Event(c.Structure):
    _fields_=[('action',c.c_int),('param',c.c_int)]
class Row(c.Structure):
    _fields_=[('title',c.c_char*128),('subtitle',c.c_char*192),('badge',c.c_char*48),('note',c.c_char*384),('tone',c.c_uint32),('event',Event),('disabled',c.c_int)]
class Button(c.Structure):
    _fields_=[('label',c.c_char_p),('event',Event),('disabled',c.c_int),('danger',c.c_int)]
class View(c.Structure):
    _fields_=[(x,c.c_int) for x in ['page','section','row_count','button_count','section_count','en','dirty']]+[
        ('title',c.c_char_p),('subtitle',c.c_char_p),('sections',c.c_char_p*3),('primary',c.c_char_p),
        ('empty_title',c.c_char_p),('empty_body',c.c_char_p),('empty_path',c.c_char_p),('status',c.c_char_p),('status_tone',c.c_uint32),
        ('rows',Row*256),('buttons',Button*6),('dialog',c.c_int),('dialog_title',c.c_char_p),('dialog_body',c.c_char_p),('dialog_confirm',c.c_char_p),('dialog_scroll',c.c_int)]
class State(c.Structure):
    _fields_=[(x,c.c_int) for x in ['page','section','selected','scroll','focus','button']]
Rect=c.CFUNCTYPE(None,c.c_void_p,c.c_int,c.c_int,c.c_int,c.c_int,c.c_uint32)
Text=c.CFUNCTYPE(None,c.c_void_p,c.c_int,c.c_int,c.c_int,c.c_char_p,c.c_uint32)
Measure=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_char_p,c.c_int)
Hit=c.CFUNCTYPE(None,c.c_void_p,c.c_int,c.c_int,c.c_int,c.c_int,Event)
class Canvas(c.Structure):
    _fields_=[('ctx',c.c_void_p),('rect',Rect),('text',Text),('measure',Measure),('hit',Hit)]

def u(s): return s.encode('utf-8')
def rgb(v): return (v&255,(v>>8)&255,(v>>16)&255)
def row(v,title,sub,badge,note=''):
    r=v.rows[v.row_count];v.row_count+=1
    r.title=u(title);r.subtitle=u(sub);r.badge=u(badge);r.note=u(note or sub);r.tone=0xffc4e98d
def buttons(v,labels):
    v.button_count=len(labels)
    for i,t in enumerate(labels): v.buttons[i].label=u(t)
def fixture(page,en):
    v=View();v.page=page;v.en=en
    titles=['Overview','Script mods','DLC library','Graphics','Saves & backups','Advanced tools'] if en else ['总览','脚本模组','DLC 管理','画质设置','存档与备份','高级工具']
    v.title=u(titles[page]);v.subtitle=u('Preview with sample data / Not connected to a Switch' if en else '界面预览 · 示例数据，未连接 Switch 实机')
    v.status=u('Preview fixture / Native C renderer' if en else '示例数据 · 使用实际 C 界面代码渲染');v.primary=u('A  Review selection' if en else 'A  查看确认内容')
    v.empty_title=u('No external scripts yet' if en else '还没有外部脚本');v.empty_body=u('Place .nsc files below, then choose Rescan. Built-in mods need no file copying.' if en else '把 .nsc 文件放入下方目录，再选择「重新扫描」。也可以切到「内置模组」直接安装。');v.empty_path=b'sdmc:/switch/gta5save/script/'
    if page==0:
        for i in range(1,5): row(v,titles[i],['','Built-in mods and stock recovery','Import and manage add-ons','Choose a preset or fine-tune','Continue your story'][i] if en else ['','内置模组、外部脚本与官方恢复','导入扩展内容，查看注册状态','选择适合你的预设，或逐项调整','继续不同进度，找回历史存档'][i],'')
    elif page==1:
        v.section_count=3
        for i,t in enumerate(['Built-in mods','External scripts','Restore stock'] if en else ['内置模组','外部脚本','恢复官方']):v.sections[i]=u(t)
        row(v,'MEGATARD v0.9.9' if en else 'MEGATARD v0.9.9 汉化','By Geekmaxxer / 2 scripts' if en else '作者 Geekmaxxer / 2 个脚本文件','Bundled' if en else '已内置','Conflicts with classic ragemenu.\nConflict checks run automatically. Restart the game after installation.' if en else '与经典 ragemenu 互斥。\n安装会覆盖同名脚本。互斥检测与回读校验自动执行，完成后重启游戏。')
        row(v,'Classic ragemenu' if en else '经典 ragemenu 简体','By maritoguionyo / 2 scripts' if en else '作者 maritoguionyo / 2 个脚本文件','Bundled' if en else '已内置')
        row(v,'HotCoffee + Zombies' if en else '热咖啡 + 丧尸','By Je11yb0ne / CinnamonCoffee','Bundled' if en else '已内置')
        row(v,'HotCoffee' if en else '热咖啡（不含丧尸）','By Je11yb0ne / CinnamonCoffee','Bundled' if en else '已内置')
        buttons(v,['Install selected','Rescan','Folder & status'] if en else ['安装所选','重新扫描','目录与状态'])
    elif page==2:
        v.section_count=2;v.sections[0]=u('Installed' if en else '已安装');v.sections[1]=u('Ready to import' if en else '待导入')
        for name,badge in [('Los Santos Classics','Registered'),('Retro Garage','Registered'),('Coastal Road','Map issue')]:row(v,name,'48.2 MB / Models 6 Maps 0 Textures 8',badge,'Folder: classics\nSpawn: classic01\nRegistration updates both required configurations.' if en else '目录：classics\n刷车名：classic01\n注册会同时更新两处游戏配置。建议每次少量添加，再重启游戏验证。')
        buttons(v,['Register / off','Rescan','Inspect','Convert','Hide HD','Delete DLC'] if en else ['注册 / 注销','重新扫描','检查资源','转换格式','屏蔽高清贴图','删除 DLC'])
    elif page==3:
        v.section_count=3
        for i,t in enumerate(['Presets','Fine-tune','Import XML'] if en else ['画质预设','逐项调整','导入配置']):v.sections[i]=u(t)
        for title,sub,badge in ([('Low / stock clocks','Minimum effects, suitable for stock clocks','Stock clocks'),('Medium / balanced','Official defaults, balanced visuals','Preset'),('Low + shadows','Keep shadows with lower effects','Preset'),('High / overclock','High settings for overclocked systems','Overclock'),('VSync off','Disable vertical sync only','Preset'),('All minimum','VSync off, all effects minimum','Preset')] if en else [('低画质（默频）','最低特效，适合默认频率使用','默频适用'),('中画质（默认）','官方默认画质，均衡选择','预设'),('低画质 + 阴影','低特效保留阴影，画面更有层次','预设'),('高画质（极限超频）','全特效，仅限极限超频使用','需超频'),('关闭 VSync','只关闭垂直同步','预设'),('全项最低','关闭 VSync，所有特效最低','预设')]):row(v,title,sub,badge)
        buttons(v,['Apply selected','Check paths','Rescan'] if en else ['应用所选','路径检查','重新扫描'])
    elif page==4:
        v.section_count=3
        for i,t in enumerate(['Story saves','Backups','Target slot'] if en else ['进度存档','历史备份','目标槽位']):v.sections[i]=u(t)
        for n,t in [('4%','序章初入洛圣都'),('20%','崔佛登场'),('31.6%','佩立托银行前夕'),('61.1%','终局大干一票'),('100%','完美全通关')]:row(v,n+' Story progress' if en else n+' '+t,'Write to the selected target slot' if en else '确认后写入当前目标槽位','Bundled' if en else '内置')
        buttons(v,['Write selected','Refresh','Save pending'] if en else ['写入所选','刷新列表','保存待写入修改']);v.buttons[2].disabled=1
    else:
        v.section_count=2;v.sections[0]=u('Parameters' if en else '参数调整');v.sections[1]=u('Diagnostics' if en else '维护诊断')
        for name,value in [('Vehicle pool','256'),('Pedestrian pool','200'),('Streaming budget','512'),('Traffic density','100')]:row(v,name,'gameconfig.xml / 0 ~ 1024',value)
        buttons(v,['Write changes','Decrease','Increase','Restore stock','Diagnose paths'] if en else ['写入参数','减小数值','增大数值','恢复原版','路径自检']);v.buttons[0].disabled=1
    return v

def main():
    a=argparse.ArgumentParser();a.add_argument('--library',required=True);a.add_argument('--out',required=True);a.add_argument('--font');args=a.parse_args()
    out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
    candidates=[args.font,r'C:\Windows\Fonts\msyh.ttc','/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc']
    font_path=next((x for x in candidates if x and Path(x).exists()),None)
    if not font_path:raise RuntimeError('Pass --font with a CJK-capable TrueType font')
    lib=c.CDLL(str(Path(args.library).resolve()));lib.ui_render.argtypes=[c.POINTER(Canvas),c.POINTER(State),c.POINTER(View)]
    fonts={};image=draw=None
    def font(n):
        if n not in fonts:fonts[n]=ImageFont.truetype(font_path,n)
        return fonts[n]
    @Rect
    def rect(ctx,x,y,w,h,col):draw.rectangle((x,y,x+w-1,y+h-1),fill=rgb(col))
    @Text
    def text(ctx,x,y,size,s,col):
        f=font(size);string=s.decode('utf-8')
        # Native draw_text uses y+size as the baseline. Works with Pillow 7+.
        ascent=f.getmetrics()[0]
        draw.text((x,y+size-ascent),string,font=f,fill=rgb(col))
    @Measure
    def measure(ctx,s,size):
        f=font(size);string=s.decode('utf-8');return int(f.getlength(string)) if hasattr(f,'getlength') else f.getsize(string)[0]
    @Hit
    def hit(*args):pass
    canvas=Canvas(None,rect,text,measure,hit)
    names=['overview','mods','dlc','graphics','saves','advanced']
    for en in (0,1):
        for page in range(6):
            image=Image.new('RGB',(1280,720));draw=ImageDraw.Draw(image);v=fixture(page,en);s=State(page,0,0,0,0,0)
            lib.ui_render(c.byref(canvas),c.byref(s),c.byref(v));image.save(out/(names[page]+('-en' if en else '-zh')+'.png'))
        for kind in ('empty','confirm','report'):
            image=Image.new('RGB',(1280,720));draw=ImageDraw.Draw(image);v=fixture(1,en);s=State(1,0,0,0,0,0)
            if kind=='empty':v.row_count=0;v.section=1;s.section=1;v.buttons[0].disabled=1
            else:
                v.dialog=1 if kind=='confirm' else 2;v.dialog_title=u('Confirm installation?' if en else '确认安装此模组？')
                v.dialog_body=u('MEGATARD v0.9.9\nTarget: update2.rpf / script_rel.rpf\n\nThis replaces same-name game scripts. Conflict detection and verification run automatically.\n\nKeep a complete archive backup and restart the game afterwards.' if en else 'MEGATARD v0.9.9 汉化\n目标：update2.rpf / script_rel.rpf\n\n安装会覆盖同名游戏脚本。互斥检测与回读校验自动执行。\n\n请先保留完整 RPF 备份。完成后重启游戏生效。')
            lib.ui_render(c.byref(canvas),c.byref(s),c.byref(v));image.save(out/(kind+('-en' if en else '-zh')+'.png'))
    print('Rendered 18 previews using source/ui.c with sample data.')

if __name__=='__main__':main()
