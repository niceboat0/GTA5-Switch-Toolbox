# GTA5-Switch-Toolbox

GTA V Switch 移植版（Title ID `0100b00b51230000`）的多功能工具箱 NRO ——
DLC 管理、脚本 MOD 安装/还原、画质预设、内置模组，一个 .nro 全搞定。

## 功能（v6.2）

- **DLC 页**：注册/卸载 add-on 车辆 DLC（dlclist.xml + extratitleupdatedata.meta 双注册）
- **脚本 MOD 页**：安装 .nsc 脚本 mod（自动识别 RSC7 官方格式 / mod 裸格式），
  一键还原官方脚本，内置 4 套 mod（互斥关系自动检查）
- **画质页**：一键切换 settings.xml 预设（低/中/高/极限超频专用）
- **性能页**：gameconfig 调参（已结案：瓶颈是 Tegra X1，超频才是正解）
- **工具页**：内置 5 个进度存档（4% ~ 100%）、信息查看

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

## 安装到 Switch

游戏文件位于 `atmosphere/contents/0100b00b51230000/romfs/`，
工具直接在 Switch 上读写 SD 卡里的 RPF（写完自动 `fsdevCommitDevice`）。

## 致谢

- MEGATARD / ragemenu / HotCoffee-NX 的原作者与汉化者
- GTA V Switch 移植社区（Geekmaxxer 教程站）
