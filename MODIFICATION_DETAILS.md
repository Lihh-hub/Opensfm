# OpenSfM 性能修改位置与作用说明

本文档记录相对于 OpenDroneMap OpenSfM 0.5.2 基线提交 `7eae7dc` 的性能修改。
当前说明对应提交 `0f23697`。文中的行号用于快速定位当前版本；后续代码变化时，
应优先通过函数名、类名或配置名定位。

## 1. 修改范围概览

| 优化方向 | 主要语言 | 主要作用 |
| --- | --- | --- |
| 图像候选配对 | Python | 避免无 GPS 或无筛选策略时退化为全图两两匹配 |
| OpenCV 线程调度 | Python | 避免多进程和 OpenCV 内部线程相互争抢 CPU |
| 中间文件存储 | Python | 降低 NPZ 和 gzip 压缩造成的 CPU 与等待时间 |
| 原生编译配置 | CMake/Python | 启用可控 AVX、原生 CPU 优化和并行构建 |
| WORDS 特征匹配 | C++ | 降低倒排索引、距离计算和结果构造开销 |
| Track 构建 | Python/C++ | 将 Union-Find 和 Track 组装从 Python 移入 C++ |
| Bundle Adjustment | C++/Python 配置 | 允许按 BA 类型调整求解器、迭代数和收敛容差 |
| RANSAC 内存分配 | C++ | 复用容器并提前分配容量 |
| Depthmap/PatchMatch | C++ | 将 patch 统计改为积分图查询并缓存权重 |

本轮共涉及 28 个文件，代码统计为约 780 行新增、128 行删除。

## 2. 图像候选配对优化

### 2.1 配置入口

文件：`opensfm/config.py`

| 当前行号 | 配置项 | 默认值 | 作用 |
| --- | --- | --- | --- |
| 172 | `matching_default_neighbors` | `20` | 所有筛选策略都未产生候选对时，按拍摄顺序选择邻近图像 |
| 175 | `matching_no_gps_neighbors` | `20` | 仅为缺少 GPS 的图像补充顺序邻居 |

### 2.2 核心实现

文件：`opensfm/pairs_selection.py`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 551 | `match_candidates_by_order()` | 根据图像顺序生成固定邻居数量的候选对 |
| 571 | `match_candidates_from_metadata()` | 汇总 GPS、时间、顺序等候选配对策略 |
| 600-601 | 配置读取 | 读取默认邻居数和无 GPS 邻居数 |
| 630 | 缺失 GPS 分支 | 仅对缺少 GPS 的图像补充顺序邻居 |
| 666 | `fallback` 分支 | 筛选结果为空时使用顺序邻居，而不是直接全图配对 |
| 698 | 报告生成 | 增加 `num_pairs_fallback` 统计字段 |

具体作用：

1. 原逻辑在 GPS 不可用或配对策略没有结果时，可能构造接近
   `N * (N - 1) / 2` 个图像对。
2. 新逻辑默认让每张图只连接附近 20 张图，大幅降低特征匹配次数。
3. 部分图像缺少 GPS 时，不再导致整个数据集放弃 GPS 配对。
4. 有 GPS 的子集继续使用地理位置筛选，缺 GPS 的图像使用拍摄顺序保持连通性。
5. `matching_default_neighbors: 0` 可以恢复原先的全图配对回退方式。

适用场景：

- 无人机连续航拍、图像文件顺序基本等于拍摄顺序。
- 图像数量较多，特征匹配是主要耗时阶段。

注意事项：

- 对无序导入、多个独立相机混合或文件名顺序失真的数据，应提高邻居数。
- 推荐先在 `12-30` 范围测试注册率和运行时间。

## 3. OpenCV 线程调度优化

文件：`opensfm/context.py`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 44 | `parallel_map()` | 根据任务数量和进程数量决定 OpenCV 线程策略 |
| 55 | 多任务分支 | 多进程/多任务运行时调用 `cv2.setNumThreads(1)` |
| 66 | `finally` 分支 | 任务结束后恢复原 OpenCV 线程数 |

具体作用：

- 单任务运行时保留 OpenCV 内部并行能力。
- 多任务运行时限制每个任务内部的 OpenCV 线程数。
- 避免出现“进程数 × OpenCV 线程数”远高于 CPU 核数的情况。
- 使用 `try/finally`，即使任务异常也会恢复线程设置。

## 4. 中间文件存储优化

### 4.1 配置入口

文件：`opensfm/config.py`

| 当前行号 | 配置项 | 默认值 | 作用 |
| --- | --- | --- | --- |
| 375 | `intermediate_storage` | `FAST` | 使用未压缩 NPZ 保存特征和视觉词 |
| 378 | `matches_gzip_compresslevel` | `1` | 使用低压缩等级保存匹配文件 |

### 4.2 特征和视觉词

文件：`opensfm/features.py`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 95 | `FeaturesData.save()` | 根据 `intermediate_storage` 选择 NPZ 保存方式 |
| 113-116 | 保存函数选择 | `COMPRESSED` 使用 `np.savez_compressed`，其他值使用 `np.savez` |

文件：`opensfm/dataset.py`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 321 | `DataSet.save_features()` | 将存储配置传给特征保存逻辑 |
| 335 | `DataSet.save_words()` | 视觉词文件也支持快速未压缩 NPZ |

### 4.3 匹配文件

文件：`opensfm/dataset.py`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 383 | `DataSet.save_matches()` | 保存匹配结果时读取 gzip 压缩等级 |
| 387-391 | gzip 写入 | 将压缩等级限制在 `0-9` 并传给 `gzip.GzipFile` |

具体作用：

- 减少特征提取、视觉词和匹配阶段用于压缩的 CPU 时间。
- 减少保存大量中间结果时的阻塞等待。
- `np.load` 可以同时读取压缩和未压缩 NPZ，因此读取接口保持兼容。

兼容旧存储行为：

```yaml
intermediate_storage: COMPRESSED
matches_gzip_compresslevel: 9
```

代价：`FAST` 模式会使用更多磁盘空间。

## 5. CMake 和原生编译优化

### 5.1 OpenSfM CMake 配置

文件：`opensfm/src/CMakeLists.txt`

| 当前行号 | 选项/位置 | 修改内容 |
| --- | --- | --- |
| 45 | `OPENSFM_ENABLE_AVX` | 默认开启 VLFeat AVX 内核 |
| 50 | `OPENSFM_NATIVE_ARCH` | 新增原生 CPU 指令集优化开关，默认关闭 |
| 51-53 | Release 编译参数 | 非 Windows 平台按需加入 `-march=native` |
| 111 | `OPENSFM_BUILD_TESTS` | 默认从开启改为关闭 |

作用：

- 生产构建默认不编译单元测试，减少构建时间和测试依赖。
- 固定部署机器可以选择针对本机 CPU 生成更高效指令。
- `-march=native` 默认关闭，避免构建产物在其他 CPU 上无法运行。

### 5.2 VLFeat AVX

文件：`opensfm/src/third_party/vlfeat/CMakeLists.txt`

| 当前行号 | 位置 | 修改内容 |
| --- | --- | --- |
| 3-6 | CPU 检测 | 判断目标处理器是否为 x86/x64 |
| 8 | AVX 条件 | 仅在开启 AVX 且目标为 x86 时启用 |
| 10-12 | 文件级参数 | 只为 `vl/mathop_avx.c` 添加 `/arch:AVX` 或 `-mavx` |
| 14 以后 | 回退分支 | 非 x86 或禁用 AVX 时定义 `VL_DISABLE_AVX` |

这样可以避免把整个工程都编译为 AVX，也避免 ARM 平台尝试编译 x86 AVX 文件。

### 5.3 Python 构建入口

文件：`setup.py`

| 当前行号 | 位置 | 修改内容 |
| --- | --- | --- |
| 33 | `cmake_command` | 集中构造 CMake 配置命令 |
| 43 | `OPENSFM_CMAKE_ARGS` | 允许通过环境变量传入额外 CMake 参数 |
| 58 | Windows 构建 | 使用 `--parallel <CPU 数量>` 并行编译 |

本机优化构建示例：

```bash
OPENSFM_CMAKE_ARGS="-DOPENSFM_NATIVE_ARCH=ON" python3 setup.py build
```

关闭 AVX 的可移植构建：

```bash
OPENSFM_CMAKE_ARGS="-DOPENSFM_ENABLE_AVX=OFF" python3 setup.py build
```

## 6. WORDS 特征匹配优化

文件：`opensfm/src/features/src/matching.cc`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 34 | `MatchUsingWords()` | 重写词袋候选索引和结果构造路径 |
| 38-39 | 倒排索引 | 使用 `unordered_map<int, vector<int>>` 并提前 `reserve()` |
| 74 | 匹配结果 | 使用 `vector<array<int, 2>>` 并提前预留容量 |
| 75 | Lowe 比率 | 预先计算 `lowes_ratio * lowes_ratio` |
| 97 | Python 绑定入口 | 计算期间通过 `py::gil_scoped_release` 释放 GIL |

具体作用：

- 哈希索引替代 `std::multimap`，减少树结构插入和查找开销。
- 先在连续的 C++ `vector` 中收集结果，再一次性构造 NumPy 数组。
- 使用平方 L2 距离，避免每次距离比较调用 `sqrt()`。
- Lowe ratio 同样平方，因此筛选语义不变。
- 释放 GIL 后，其他 Python 线程可以继续工作。

## 7. Track 构建迁移到 C++

这是本次新增代码量最大的一项优化。

### 7.1 Python 调用入口

文件：`opensfm/tracking.py`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 61 | `create_tracks_manager()` | 优先调用 C++ `pymap.create_tracks_manager()` |
| 70-71 | 能力检测 | 使用 `hasattr` 检查新接口是否存在 |
| 后续原实现 | Python 回退 | 旧扩展库没有新接口时继续使用 Python Union-Find |

保留回退路径是为了兼容尚未重新编译的 OpenSfM 扩展。

### 7.2 C++ 主实现

新增文件：`opensfm/src/map/src/tracks_manager_builder.cc`

| 当前行号 | 类/函数/位置 | 作用 |
| --- | --- | --- |
| 20 | `NumericType` | 枚举支持的 NumPy 数值类型 |
| 30 附近 | 数组描述逻辑 | 缓存 dtype、数据地址、形状和 stride |
| 89-101 | 数值读取 | 支持 `double/float/int/int64/uint8/uint16/int16` |
| 146 | `CreateTracksManager()` | C++ Track 构建总入口 |
| 151-172 | 输入预处理 | 缓存图像数据并将图像名映射为整数 ID |
| 171 以后 | 节点索引 | 将 `(image_id, feature_id)` 映射为 Union-Find 节点 |
| 222 | 匹配边收集 | 根据匹配数量提前扩充分配容量 |
| 231 | GIL 释放 | Union-Find、分组和 Track 构造期间释放 Python GIL |
| 236-243 | Track 分组 | 按 Union-Find 根节点聚合并预留容器 |

新增头文件：`opensfm/src/map/tracks_manager_builder.h`

- 第 8 行声明 `CreateTracksManager()`。

C++ 实现完成以下工作：

1. 解析特征坐标、颜色、语义分割、实例 ID 和匹配数组。
2. 使用整数节点和按秩合并、路径压缩的 Union-Find。
3. 将成对特征匹配聚合成完整 Track。
4. 删除长度小于 `min_track_length` 的 Track。
5. 删除同一 Track 中包含同一图像多个特征的无效 Track。
6. 对 Track 和 Observation 排序，保证输出顺序稳定。
7. 直接构造 `TracksManager` 和 `Observation`，减少 Python 对象循环。
8. 通过 NumPy stride 读取数据，支持非连续数组。
9. 跳过形状为 `(0,)` 的空匹配数组。

### 7.3 编译和 Python 绑定

| 文件 | 当前位置 | 修改作用 |
| --- | --- | --- |
| `opensfm/src/map/CMakeLists.txt` | 第 38 行 | 将 `tracks_manager_builder.cc` 加入 map 模块 |
| `opensfm/src/map/python/pybind.cc` | 第 13、299 行 | 引入头文件并注册 `create_tracks_manager` |
| `opensfm/src/map/pymap.pyi` | 第 18 行 | 增加 Python 类型声明 |

### 7.4 测试覆盖

文件：`opensfm/test/test_datastructures.py`

| 当前行号 | 测试 | 覆盖内容 |
| --- | --- | --- |
| 1129 | `test_create_tracks_manager_from_arrays()` | C++ 数组入口及 Track 结果 |

覆盖数据包括 `float32` 特征、`uint8` 颜色/分割、`int16` 实例 ID、
两个有效 Track，以及形状为 `(0,)` 的空匹配。

## 8. Bundle Adjustment 参数化

### 8.1 配置入口

文件：`opensfm/config.py`

| 当前行号 | 配置项 | 默认值 |
| --- | --- | --- |
| 267 | `bundle_linear_solver_type` | `SPARSE_SCHUR` |
| 269 | `bundle_function_tolerance` | `1e-6` |
| 272 | `local_bundle_max_iterations` | `10` |
| 273 | `local_bundle_linear_solver_type` | `DENSE_SCHUR` |
| 275 | `bundle_shot_poses_max_iterations` | `10` |
| 276 | `bundle_shot_poses_linear_solver_type` | `DENSE_QR` |

### 8.2 BundleAdjuster

| 文件 | 当前位置 | 修改作用 |
| --- | --- | --- |
| `opensfm/src/bundle/bundle_adjuster.h` | 第 247、351 行 | 声明 setter 并保存 `function_tolerance_` |
| `opensfm/src/bundle/src/bundle_adjuster.cc` | 第 40 行 | 默认容差初始化为 `1e-6` |
| 同上 | 第 366 行 | 实现 `SetFunctionTolerance()` |
| 同上 | 第 1086 行 | 写入 Ceres `options.function_tolerance` |
| `opensfm/src/bundle/python/pybind.cc` | 第 102 行 | 向 Python 暴露 setter |
| `opensfm/src/bundle/pybundle.pyi` | 第 60 行 | 增加类型声明 |

### 8.3 不同 BA 场景

文件：`opensfm/src/sfm/src/ba_helpers.cc`

| 当前行号 | BA 场景 | 使用配置 |
| --- | --- | --- |
| 263-266 | Local Bundle | `local_bundle_max_iterations`、`bundle_function_tolerance`、`local_bundle_linear_solver_type` |
| 543-547 | Shot Poses Bundle | `bundle_shot_poses_max_iterations`、`bundle_function_tolerance`、`bundle_shot_poses_linear_solver_type` |
| 724-727 | Global Bundle | `bundle_max_iterations`、`bundle_function_tolerance`、`bundle_linear_solver_type` |

作用：将原先硬编码的求解器和迭代次数改为配置项，使 ODM 可以针对数据规模、
精度要求和运行时间分别调节三类 BA。

## 9. RANSAC 和鲁棒估计内存优化

| 文件 | 当前行号 | 修改内容 | 作用 |
| --- | --- | --- | --- |
| `opensfm/src/robust/model.h` | 19 | `errors.reserve(end - begin)` | 避免误差数组反复扩容 |
| `opensfm/src/robust/random_sampler.h` | 18 | `random_samples.reserve(size)` | 避免随机样本数组反复扩容 |
| `opensfm/src/robust/robust_estimator.h` | 73-79 | LO 循环外创建并预留 `inliers_samples`，循环内 `clear()` | 复用局部优化样本缓冲区 |

这些修改不改变 RANSAC 的采样、评分和模型选择逻辑，只减少高频循环中的堆内存申请。

## 10. Depthmap/PatchMatch 优化

### 10.1 数据成员

文件：`opensfm/src/dense/depthmap.h`

| 当前行号 | 成员 | 作用 |
| --- | --- | --- |
| 112 | `patch_sum_` | 保存参考图像积分图 |
| 113 | `patch_squared_sum_` | 保存参考图像平方积分图 |
| 114 | `patch_spatial_weights_` | 缓存 patch 内空间权重 |
| 115 | `patch_color_weights_` | 缓存 256 种像素差对应的颜色权重 |

### 10.2 核心实现

文件：`opensfm/src/dense/src/depthmap.cc`

| 当前行号 | 函数/位置 | 修改内容 |
| --- | --- | --- |
| 142 | 初始化阶段 | 调用 `UpdatePatchWeights()` |
| 158 | 图像设置阶段 | 使用 `cv::integral()` 构建积分图和平方积分图 |
| 178-180 | `SetPatchSize()` | patch 大小变化时重新生成权重表 |
| 274 | `PatchVariance()` | 使用四次积分图访问计算 patch 方差 |
| 499-500 | 权重读取 | 直接查询颜色权重表和空间权重表 |
| 503 | `UpdatePatchWeights()` | 预计算颜色与空间双边权重 |

具体作用：

- Patch 方差从遍历 patch 中所有像素改为 O(1) 积分图查询。
- PatchMatch 内层循环不再为每个像素差重复调用 `exp()`。
- 删除原先用于方差计算的临时 patch 缓冲区。

注意：ODM 如果使用 OpenMVS 完成密集重建，则不会进入 OpenSfM 自带的
Depthmap/PatchMatch 路径，这项优化不会影响该流程。

## 11. 文档和仓库结构

| 文件 | 修改内容 |
| --- | --- |
| `PERFORMANCE.md` | 新增推荐参数、编译方式、兼容开关和验证指标 |
| `README.md` | 增加性能文档入口 |
| `.gitmodules` / `opensfm/src/third_party/pybind11` | pybind11 保持为 Git 子模块 |

克隆仓库时应使用：

```bash
git clone --recurse-submodules git@github.com:Lihh-hub/Opensfm.git
```

## 12. 推荐 ODM 配置

可先使用以下配置测试连续无人机航拍数据：

```yaml
processes: 8
matching_default_neighbors: 20
matching_no_gps_neighbors: 20
intermediate_storage: FAST
matches_gzip_compresslevel: 1
bundle_linear_solver_type: SPARSE_SCHUR
bundle_function_tolerance: 1e-6
local_bundle_max_iterations: 10
local_bundle_linear_solver_type: DENSE_SCHUR
bundle_shot_poses_max_iterations: 10
bundle_shot_poses_linear_solver_type: DENSE_QR
```

实际 `processes` 应根据 CPU 核数和可用内存调整。

## 13. 建议验证指标

性能指标：

- OpenSfM 总运行时间。
- `detect_features`、`match_features`、`create_tracks`、`reconstruct` 各阶段时间。
- 候选图像对数量和实际匹配对数量。
- CPU 使用率、峰值内存和中间文件磁盘占用。

质量指标：

- 成功注册图像数量和比例。
- 重建点数量。
- 平均/中位重投影误差。
- GCP 误差和 GPS 对齐误差。
- 正射影像完整度、破洞和明显错位。

建议使用同一数据集、同一 ODM 参数和同一机器，对基线提交 `7eae7dc`
与当前提交进行对比。

## 14. 修改提交对应关系

| 提交 | 内容 |
| --- | --- |
| `7eae7dc` | 导入 OpenDroneMap OpenSfM 源码基线 |
| `825cb4c` | 图像配对、线程调度和中间存储优化 |
| `4951684` | 原生编译和 WORDS 特征匹配优化 |
| `8710dbe` | Track 构建迁移到 C++ |
| `2ad521f` | Bundle Adjustment 和鲁棒估计优化 |
| `bc955ab` | Depthmap patch 统计与权重优化 |
| `0f23697` | 修复 AVX 编译范围和空匹配数组处理 |

## 15. 当前验证状态

根据修改时的要求，本轮没有执行编译、单元测试或 ODM 完整数据集测试。
已经完成源码静态检查和 Git 差异检查。因此这些优化需要在实际 ODM 环境中重点验证：

1. C++ 扩展能否在目标 Linux/ODM 镜像中正常编译。
2. AVX 开启与关闭时的兼容性。
3. C++ Track 构建结果与旧 Python 实现是否一致。
4. 邻居配对数量降低后，困难数据集的注册率是否下降。
5. BA 参数在大规模数据集上的速度和精度平衡。

