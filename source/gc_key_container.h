/* gc_key_container.h —— 自动生成，请勿手改
 *
 * ★ key → gameconfig 里【正确容器路径】的映射
 *
 * 为什么需要它：
 *   gameconfig.xml 的 switch 段是有层级的：
 *     <Config type="CGameConfig">
 *       <ConfigPopulation> ... </ConfigPopulation>      ← 密度/刷新率/剔除
 *       <ConfigModelInfo>  ... </ConfigModelInfo>       ← 模型池
 *       <ConfigExtensions> ... </ConfigExtensions>      ← 扩展池
 *     </Config>
 *   新增项必须插到【对应容器内】，否则游戏读不到，
 *   表现为「预设套了但没效果」（而手动改已存在的项有效）。
 *
 * 路径格式: 用 '/' 分隔，相对 <Config type="CGameConfig"> 的第一层子容器。
 *   例: "ConfigPopulation/VehicleSpacing"
 */
#ifndef GC_KEY_CONTAINER_H
#define GC_KEY_CONTAINER_H

typedef struct { const char *key; const char *path; } GcKeyPath;

/* 按 key 名排序，便于二分/线性查找 */
static const GcKeyPath GC_KEY_PATHS[] = {
    { "DensityBasedRemovalRateScale", "ConfigPopulation" },
    { "DensityBasedRemovalRateScale_Base", "ConfigPopulation" },
    { "DensityBasedRemovalTargetHeadroom", "ConfigPopulation" },
    { "DensityBasedRemovalTargetHeadroom_Base", "ConfigPopulation" },
    { "MaxCompEntityModelInfos", "ConfigModelInfo" },
    { "MaxDoorExtensions", "ConfigExtensions" },
    { "MaxExpressionExtensions", "ConfigExtensions" },
    { "MaxExtraPedModelInfos", "ConfigModelInfo" },
    { "MaxExtraVehicleModelInfos", "ConfigModelInfo" },
    { "MaxMloInstances", "ConfigModelInfo" },
    { "MaxSpawnPointOverrideExtensions", "ConfigExtensions" },
    { "MaxTotalPeds", "ConfigPopulation" },
    { "MaxTotalPeds_Base", "ConfigPopulation" },
    { "PedMemoryMultiplier", "ConfigPopulation" },
    { "PedPopulationFrameRate", "ConfigPopulation" },
    { "PedPopulationFrameRate_Base", "ConfigPopulation" },
    { "PlayersRoadScanDistance", "ConfigPopulation" },
    { "PlayersRoadScanDistance_Base", "ConfigPopulation" },
    { "VehicleCullRange", "ConfigPopulation" },
    { "VehicleCullRangeOffScreen", "ConfigPopulation" },
    { "VehicleCullRangeOffScreen_Base", "ConfigPopulation" },
    { "VehicleCullRangeOnScreenScale", "ConfigPopulation" },
    { "VehicleCullRangeOnScreenScale_Base", "ConfigPopulation" },
    { "VehicleCullRange_Base", "ConfigPopulation" },
    { "VehicleMaxCreationDistance", "ConfigPopulation" },
    { "VehicleMaxCreationDistanceOffscreen", "ConfigPopulation" },
    { "VehicleMaxCreationDistanceOffscreen_Base", "ConfigPopulation" },
    { "VehicleMaxCreationDistance_Base", "ConfigPopulation" },
    { "VehicleMemoryMultiplier", "ConfigPopulation" },
    { "VehicleParkedUpperLimit", "ConfigPopulation" },
    { "VehicleParkedUpperLimit_Base", "ConfigPopulation" },
    { "VehiclePopulationFrameRate", "ConfigPopulation" },
    { "VehiclePopulationFrameRate_Base", "ConfigPopulation" },
    { "VehicleSpacing_1", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_10", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_11", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_12", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_13", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_14", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_15", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_2", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_3", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_4", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_5", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_6", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_7", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_8", "ConfigPopulation/VehicleSpacing" },
    { "VehicleSpacing_9", "ConfigPopulation/VehicleSpacing" },
    { "VehicleTimesliceMaxUpdatesPerFrame", "ConfigPopulation" },
    { "VehicleTimesliceMaxUpdatesPerFrame_Base", "ConfigPopulation" },
    { "VehicleUpperLimit", "ConfigPopulation" },
    { "VehicleUpperLimit_Base", "ConfigPopulation" },
};
#define GC_NUM_KEY_PATHS ((int)(sizeof(GC_KEY_PATHS)/sizeof(GC_KEY_PATHS[0])))

#endif /* GC_KEY_CONTAINER_H */
