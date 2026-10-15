# 电台安装与测试

本版本接入 Geekmaxxer 分支的电台安装后端，保留本项目现代界面。**运行包未内置**。作者 [Self Radio v1.1 下载页](https://buzzheavier.com/gt9o86vuw4kl) 检查时为空，暂时无法提供开箱即用的完整电台包。

## 自定义电台

需要 Python 3.11+、Self Radio NX v1.1 的 HARD 目录及配套 `release-verification.json`。准备时检查全部 16 个归档替换、subsdk4 及 main.npdm；来源与记录不一致会拒绝导出。工具不会判断当前安装的游戏构建，安装前须人工核对：v1.1 配套构建 `6e3e9b3af5746e74b615bd981080c9271bc226620`。

```powershell
python tools/export_package.py selfradio --input PATH/TO/HARD --manifest PATH/TO/release-verification.json --output self-radio-package
```

将整个输出目录复制到 `SD:/switch/gta5save/packages/self-radio-package/`，其中应包含 `package.txt` 和 `payload/`。工具侧栏「电台 → 自定义电台 → 重新扫描 → 安装所选」。音乐目录为 `SD:/atmosphere/contents/0100b00b51230000/romfs/User Music/`。安装运行包后重启游戏，在游戏中测试；不能直接以 MP3 代替安装包。

## 替换原电台

先在电脑端 [GTA Radio Editor](https://github.com/Geekmaxxer/gta-radio-editor) 生成电台 RPF 和配套 audio_rel.rpf。工具本身不编码 AWC。查看实际参数：

```powershell
python tools/export_package.py radio --help
```

两个输出必须成对导出，再放到同一 packages 根目录，从「电台 → 替换原电台」安装。

## 备份与恢复

操作前保留完整 update.rpf / update2.rpf 备份。临时文件和自动备份各可能需要一份完整 RPF 的空间。安装器检查来源 SHA256 和目标原文，拒绝覆盖未知改动。备份及事务记录位于 `packages/.backups/`，勿在测试结束前删除。

用「还原备份」恢复本工具安装前的内容；若之后又修改了相同文件，恢复会拒绝覆盖。断电或异常退出后先运行「中断恢复」。修改高级参数或 DLC 后，须先保存再安装电台，以免缓存指向旧归档。

测试顺序：确认游戏原本正常 → 安装单个包 → 重启验证 → 测试歌曲播放、切换和存读档 → 还原备份 → 再次验证原版。当前仅做主机临时 RPF 测试和 NRO 编译，尚未完成实机验证。

## 来源记录

后端及导出器来源：[Geekmaxxer 分支 a39a86e](https://github.com/Geekmaxxer/GTA5-Switch-Toolbox/tree/a39a86e03e7c26859b1c2274f0fd33453c0da09f)，包含 gc_package、gc_nsc、脚本写入修复及其主机测试；本项目增加现代电台页并移植测试到 Zig / Linux。

MEGATARD v1.0.5 三个脚本取自该分支。经典英文版由用户提供、作者 maritoguionyo，原始文件 SHA256：`3ce8f1945aedf78d26c9182db3428ee41754a2e46f7de235458fad86fbca30b5`；菜单本体按字节原样打包，复用经典版加载器。
