# MEGATARD v1.0.5 中文恢复

原作者：[Geekmaxxer](https://github.com/Geekmaxxer/GTA5NX-MG-Menu)。原始三文件来自其工具箱分支提交 `a39a86e03e7c26859b1c2274f0fd33453c0da09f`。本工具箱在该版本上恢复中文，属于修改版。

## 使用

1. 替换 SD 卡 `/switch/gta5save.nro` 为本工具箱 v7.2.2。
2. 在「脚本模组 → 内置模组」重新安装 **MEGATARD v1.0.5 中文恢复版**，然后重启游戏。仅替换 NRO 不会更新已安装的游戏脚本。
3. 按 **L + 十字键下** 打开菜单，进入 **Settings → Language**。
4. 选择第七项 **Chinese / 简体中文**，按 **A** 应用并保存。原有语言选择不强制重置。

菜单复用原脚本内已有的中文译文；本补丁未逐条重译全部功能、车辆名或提示。中文字体显示和真实游戏存档的持久化需要实机测试。

## 补丁范围

v1.0.5 保留中文字符串，但翻译分发会覆盖中文参数，语言页也只列出六种语言。本补丁恢复中文参数，并把中文追加为语言编号 6。英语、西班牙语、德语、俄语、葡萄牙语、法语的原有编号与译文分发保持一致。

修改包括翻译分发函数、语言行绘制函数、语言页条目数量、选择边界、原有四位存储槽的写入/读取边界。七个语言名称追加到最后一页字符串数据。所有函数入口地址、代码长度、原有字符串偏移和 native 表保持不变，其他游戏功能字节码保持原样；两个配套加载脚本沿用原版。

这是一份针对确切文件的可复现字节码补丁，不是从作者源码重新编译。补丁拒绝未知版本或已经修改的输入文件。

| 文件 | SHA256 |
|---|---|
| 原版 ragemenu.nsc | `3bf7fbe1ec86e855de3bd1af168953bb2474a416ae43b4d22cba5bb2a1ac9658` |
| 中文恢复版 ragemenu.nsc | `6c6c40b8fcd1df6b0f0bd81e0ae5a20b82998b80faec0fa956d6d6d549ea7d49` |
| achievement_controller.nsc | `717b8b19ab45b0eb47277a29ce5fafd5f6f97332449ecfb998cc175bb86afc1a` |
| shop_controller.nsc | `bda3e3eee9a5136b538a15fe5a53574ec36a7995d0c818ed3248d33380e5bbc1` |

## 复现与验证

```sh
python3 tools/restore_megatard_zh.py original/ragemenu.nsc restored/ragemenu.nsc
python3 tests/test_megatard_zh.py --output build/megatard-zh
```

39 项检查执行真实的相关字节码，覆盖原有六种语言、中文翻译参数、全部七个语言选项、A 选择、越界拒绝、原有存储编码、三个角色的保存与重新读取。绘制和游戏 stat native 使用捕获桩，不能代替实机测试。检查还根据少量原始区域快照还原并验证原版完整 SHA256，证明未修改其他原始数据；补丁结果与内置文件逐字节一致。

安装器测试另行验证 NSC 结构、三个配套文件的 RPF 写入及回读。本版本继续使用工具箱现有的安装确认和官方恢复功能。
