# GTA5-Switch-Toolbox

GTA V Switch 移植版（Title ID `0100b00b51230000`）的多功能工具箱 NRO ——
DLC 管理、脚本 MOD 安装/还原、画质预设、内置模组，一个 .nro 全搞定。

## ⚠️ 使用前必读：备份 update2.rpf

**强烈建议先备份**，任何 RPF 写入操作（装 mod / 卸载 mod / 还原官方）都有小概率翻车：

```
SD卡:/atmosphere/contents/0100b00b51230000/romfs/update/update2.rpf
```

把它复制一份到电脑或 SD 卡其他位置（约 700MB）。出问题时把备份拷回去即可完全恢复。
工具内的「还原官方脚本」只还原 3 个脚本条目，**不能替代完整备份**。

## 功能（v6.2）

- **DLC 页**：注册/卸载 add-on 车辆 DLC（dlclist.xml + extratitleupdatedata.meta 双注册）
- **脚本 MOD 页**：安装 .nsc 脚本 mod（自动识别 RSC7 官方格式 / mod 裸格式），
  一键还原官方脚本，内置 4 套 mod（互斥关系自动检查）
- **画质页**：一键切换 settings.xml 预设（低/中/高/极限超频专用）
- **性能页**：gameconfig 调参（已结案：瓶颈是 Tegra X1，超频才是正解）
- **工具页**：内置 5 个进度存档（4% ~ 100%）、信息查看

## 各功能使用方法

### 安装与启动

1. 下载 Release 里的 `gta5save.nro`，复制到 SD 卡 `/switch/` 目录
2. 按 R 启动游戏进入 hbmenu（或关游戏回 hbmenu），选「GTA5 Switch Toolbox」启动
3. 工具内：**L/R 切换页签**，十字键上下选条目，A 确认，B 返回/退出

### 脚本 MOD 页（核心功能）

**装内置 mod**：
1. 选「★模组管理」→ 弹出二级菜单（3 个官方还原项 + 4 个内置 mod）
2. 选想要的 mod（如 MEGATARD v0.9.9 汉化）→ 会列出它将覆盖的条目
3. 确认后自动写入 script_rel.rpf，装完自动回读校验
4. 互斥的 mod（如两个 ragemenu 系）装第二个前会提示先卸载

**装外部 mod**：把 .nsc 文件放到 SD 卡 `/switch/gta5save/script/` 目录
（工具首次进入脚本页会自动创建，直接丢在 `/switch/gta5save/` 下也能扫到），
在列表里选中按 A 安装。官方格式（RSC7）和 mod 裸格式都能自动识别。

**还原官方**：模组管理里选「还原官方脚本」的对应项（error_listener /
achievement_controller / shop_controller），可单独还原任意一个。

**游戏内呼出 mod 菜单**：装好后进游戏，按 **L + 十字键下**（MEGATARD/ragemenu）或
**B + 十字键右**（热咖啡），十字键导航，A 确认，B 返回。

### DLC 页

**目录说明**：
- **已装目录**（游戏实际读取）：`/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks/`
- **待导入目录**（把 dlcpack 文件夹丢这里，工具扫描后导入）：
  `/switch/gta5save/dlc/`（推荐）。以下位置也能被扫到：
  `/switch/gta5save/`、`/switch/GTA5DLC/`、`/dlc/`、`/gta5dlc/`、`/switch/`

**使用方法**：
1. 把 PC 移植好的 dlcpack 文件夹（内含 `dlc.rpf`）放进待导入目录
2. DLC 页按 **Y** 切到「待导入列表」，选中按 **A** 导入
   （自动复制到 dlcpacks/ + 注册 dlclist.xml + extratitleupdatedata.meta）
3. 已装列表里选中按 A 可注册/注销；⚠️ PC 原格式包会标「会闪退」，别直接导入
4. 导入后建议重启游戏验证，一次别加太多包

### 画质页

选中预设按 A 一键切换 settings.xml：
- **低画质（默频使用）**：不超频的机器用这个
- **中画质（默认画质）**：超频后的平衡选择
- **高画质（极限超频专用）**：CPU 1963/GPU 768/RAM 2666 以上再用
- **★关VSync / ★★关VSync+全压零**：帧数优先的激进方案

### 工具页

- **内置存档库**：ZL 键呼出，5 个进度（4% / 20% / 31.6% / 61.1% / 100%），
  选中覆盖当前存档（会先确认）
- **信息页**：查看当前 mod 安装状态、RPF 剩余空间等

## 内置模组（romfs/builtin_mods/）

| 模组 | 作者 | 槽位 |
|---|---|---|
| MEGATARD v0.9.9 汉化 | Geekmaxxer | ragemenu |
| 经典 ragemenu 简体 v13 | maritoguionyo | ragemenu（与上互斥） |
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
