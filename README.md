# GTA5-Switch-Toolbox

GTA V Switch 移植版（Title ID `0100b00b51230000`）的多功能工具箱 NRO ——
DLC 管理、脚本 MOD 安装/还原、画质预设、内置模组，一个 .nro 全搞定。


中文 | [English](README.en.md)

## ⚠️ 使用前必读：备份 update.rpf 和 update2.rpf

**强烈建议先备份**，任何 RPF 写入操作（装 mod / 卸载 mod / 还原官方 / 性能调参写入）都有小概率翻车：

```
SD卡:/atmosphere/contents/0100b00b51230000/romfs/update/update.rpf     （约 900MB）
SD卡:/atmosphere/contents/0100b00b51230000/romfs/update/update2.rpf    （约 700MB）
```

把这两个文件都复制一份到电脑或 SD 卡其他位置。出问题时拷回去即可完全恢复：
- **update2.rpf**：脚本 mod 装在里面（script_rel.rpf）
- **update.rpf**：gameconfig（性能调参）和 dlclist.xml（DLC 注册）在里面

工具内的「还原官方脚本」只还原 3 个脚本条目，**不能替代完整备份**。

## 功能与操作（v7.0）

原生 1280×720 界面：固定侧栏、总览入口、分区列表、所选内容说明、统一操作栏。
中英文自动跟随系统语言，兼容手柄与触屏，无需额外界面库。

1. 下载构建产物 `gta5save.nro`，放到 SD 卡 `/switch/`，从 hbmenu 启动。
2. 在「总览」打开脚本模组、DLC、画质或存档；高级参数与诊断放在「高级工具」。
3. 选择列表项，查看右侧说明；按 A 或点操作栏按钮打开确认，确认后才写入。

| 操作 | 手柄 | 触屏 |
|---|---|---|
| 切换工作空间 | L / R | 点击侧栏 |
| 选择条目 | 十字键或任一摇杆上下 | 点击列表项（只选中） |
| 切换分区 | 在列表第一项按上，再按左右，按下进入列表 | 点击顶部分类 |
| 使用操作栏 | X 聚焦操作栏，左右选择，A 执行；X 或上返回列表 | 点击按钮 |
| 应用所选内容 | A，查看确认后再按 A | 点击主按钮，再点确认 |
| 调整数值 | 在逐项画质或高级参数列表中用 ZL / ZR，可长按 | 操作栏的减小 / 增大（画质） |
| 返回总览 / 取消弹窗 | B | 点击总览 / 取消按钮 |
| 退出 | +；有未保存修改时先确认 | — |

**脚本模组**：内置模组 / 外部脚本 / 恢复官方分开展示。内置模组直接选中安装；
外部 `.nsc` 放入 `/switch/gta5save/script/` 后重新扫描。同名覆盖、互斥检测与回读校验
沿用原有逻辑。游戏内呼出：MEGATARD/ragemenu 用 **L + 十字键下**，热咖啡用
**B + 十字键右**。脚本移除不会自动恢复官方，请按需要使用恢复官方分区。

**DLC 管理**：「已安装」处理注册、转换格式、屏蔽高清贴图和删除；「待导入」处理导入。
将含 `dlc.rpf` 的文件夹放到 `/switch/gta5save/dlc/` 后重新扫描。游戏读取目录为
`/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks/`。
PC 格式会明确标记并阻止直接导入；请先完成 Switch 格式转换。检查资源可查看详细报告。

**画质设置**：默认打开预设，默频适用选项排在前面。逐项调整留在内存，点击保存调整写入。
导入配置扫描 `/switch/gta5save/` 下的 XML。高画质仅适用于极限超频环境，实际帧数取决于设备。

**存档与备份**：进度存档、历史备份、目标槽位都有可见入口。
选择进度存档或备份后，确认框显示目标路径；确认会载入并写入目标槽位。
写入前备份磁盘上的原存档；同秒备份使用独立目录，备份失败则取消写入。
切换槽位不会立即覆盖存档。未找到可用目标时禁止写入，先在游戏内创建存档。

**高级工具**：手动 gameconfig 参数、恢复原版、路径自检与回读校验。
参数写入与原版恢复均须确认；报告支持上下滚动。+ 退出不自动写入尚未保存的修改。

## 内置模组（romfs/builtin_mods/）

| 模组 | 作者 | 槽位 |
|---|---|---|
| MEGATARD v0.9.9 汉化 | Geekmaxxer | ragemenu |
| 经典 ragemenu 简体 10月7日版 | maritoguionyo | ragemenu（与上互斥） |
| 热咖啡+丧尸 | Je11yb0ne/CinnamonCoffee | simple_zombies |
| 热咖啡（不含丧尸） | Je11yb0ne/CinnamonCoffee | simple_zombies（与上互斥） |

官方原版脚本（romfs/stock_scripts/）从 build 2699 原版 update2.rpf 提取，RSC7 格式。

## 构建

需要 devkitPro A64 工具链（Windows 本机无 gcc，可用 Docker 交叉编译）：

```bash
make
```

产出 `gta5save.nro`，复制到 SD 卡 `/switch/gta5save.nro`，通过 hbmenu 启动。

## 致谢

- **[Geekmaxxer](https://github.com/Geekmaxxer)** —— [MEGATARD GTA5-NX Menu SDK](https://github.com/Geekmaxxer/GTA5NX-MG-Menu)
  （MEGATARD v0.9.9 原作者）、[GTA5-NX-Tutorial](https://geekmaxxer.github.io/GTA5-NX-Tutorial/) 教程站、
  [RPF Radio Editor](https://github.com/Geekmaxxer/gta-radio-editor)
- **[Je11yb0ne](https://github.com/Je11yb0ne)** —— [HotCoffee-NX](https://github.com/Je11yb0ne/HotCoffee-NX)
  （CinnamonCoffee 的 Switch 移植，含热咖啡+丧尸两个版本）
- **maritoguionyo** —— 经典 ragemenu v13 的 Switch 移植（无公开仓库，Discord 分发）
- **[ShinyWasabi](https://github.com/ShinyWasabi)** —— [RageMenu](https://github.com/ShinyWasabi/RageMenu)
  （经典 ragemenu 的菜单底座来源）
- GTA V Switch 移植社区（Geekmaxxer 教程站、GBAtemp）
