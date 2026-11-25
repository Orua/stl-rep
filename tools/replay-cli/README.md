# STL 录像命令行工具

在仓库根目录用 PowerShell 执行。需要 Python 3.10 或更新版本。构建出的三个程序默认从 `agent/output/stl-rep-cli` 查找，可用 `--bin-dir` 指定其他位置。

```powershell
$cli = 'tools/replay-cli/stl_rep.py'
$rep = 'D:/Replays/LastReplay.rep'
$data = 'D:/StarCraftData'
$terrain = 'D:/StarCraftData/terrain.mpq'

python $cli info $rep
python $cli commands $rep --type 'Leave'
python $cli build-order $rep --to-frame 3000
python $cli extract-map $rep --output 'agent/output/stl-rep-cli/replay.chk'
python $cli simulate $rep --data $data --terrain $terrain --frame 1000 --units
python $cli simulate $rep --data $data --terrain $terrain
python $cli check 'D:/Replays' --data $data --terrain $terrain --sample 100 --seed 20261125 --jobs 4 --output 'agent/output/stl-rep-cli/my-check.jsonl'
```

- `info` 读取录像头、版本范围、地图、玩家、APM/EAPM、退场指令及估计胜队。胜队估计为 0 时表示未知。
- `commands` 按指令帧导出，支持按玩家 ID、类型和帧区间过滤。指令解码异常会附在输出末尾，标有 `parse_error`。
- `build-order` 筛选玩家发出的建造、生产、变形、科技和升级指令。实际资源、建筑数、单位数可在 `simulate --frame` 的引擎状态中查看。
- `extract-map` 写出 REP 内嵌的 `scenario.chk` 数据。输出路径必须是新文件。
- `simulate` 按网页端使用的 REP 文件头规则选择经典版或重制版 OpenBW。默认跑到录像头声明的结束帧，`--frame` 提前停在指定帧，返回 `checkpoint`。该帧状态在处理该帧指令之前读取。`--units` 额外输出参与玩家的单位快照，各项依次为单位槽位、代数、类型 ID、玩家槽位、精灵坐标 X 和 Y，快照跳过运输舱内单位和炮塔子单位。胜方来源 `engine` 表示游戏引擎判胜，`last_player_remaining` 表示根据双人局的退场情况推断。
- `check` 同时运行解析器和模拟器，对照录像声明的总帧数和引擎观察到的退场帧，并检查剩余指令字节及解析器报告的指令错误。某玩家已经有胜负结果时，引擎会消费其后续退场指令而保留玩家状态，这类退场指令记作 `exit_after_result_frame`。跑完和通过这些对照只代表当前检查通过，原版逐帧一致性需另行验证。

批量命令支持同时传入文件和多个目录，递归查找 REP。去掉 `--sample` 会扫描全部，`--jobs` 控制并发，`--timeout` 控制每个子进程的超时秒数。每份录像独立运行，单份失败后继续处理其余录像。

模拟使用本地 `StarDat.mpq`、`BrooDat.mpq`、`patch_rt.mpq`。重制版另需 `terrain.mpq`，可放到数据目录或用 `--terrain` 指定。地形资源的读取优先于经典 MPQ，与网页一致。CLI 只读取本地资源，游戏文件和高清画面资源由使用者提供。网页的绘制、音频、迷雾显示、进度跳转和资源确认属于交互层，CLI 通过逐帧模拟和停帧快照提供诊断数据。 [网页使用的地形包清单](https://dgant.github.io/mpqs/remastered/terrain-manifest.json)

批量结果为 UTF-8 JSON Lines，标准错误输出汇总，`parsed` 统计成功读出的录像数，`finished` 统计到达最后一帧的录像数。报告包含 `status`、所用引擎、错误阶段、出错帧（若已进入模拟）及两个工具各自的结果。出现失败、差异或格式不支持时返回退出码 1，参数或文件路径有误时返回 2。录像帧按 42 毫秒换算，`duration_ms` 表示录像全长，`time_ms` 表示当前模拟位置。

## 重新编译

安装 Go 1.26 和带 x64 C++ 工具的 Visual Studio。准备一份解压的 zlib 源码（已验证 1.3.1）。在新建的单独目录获取与本项目网页兼容的 Dan Gant OpenBW 源码，再应用仓库的经典版和重制版录像补丁。

```powershell
$engine = 'agent/temp/openbw-cli-source'
git clone --no-checkout https://github.com/dgant/openbw.git $engine
git -C $engine switch --detach 6285a936c386c883504094769b5cb68f03f4c9f6
git -C $engine apply (Resolve-Path 'build/patches/classic-replay-map-triggers.patch').Path
git -C $engine apply (Resolve-Path 'build/patches/remastered-replay-map-triggers.patch').Path
& 'tools/replay-cli/build.ps1' -EngineSource $engine -ZlibSource 'D:/Sources/zlib-1.3.1'
```

可用 `-VsDevCmd` 明确指定 Visual Studio 的 `VsDevCmd.bat`。构建产物位于 `agent/output/stl-rep-cli`，中间文件位于 `agent/temp/stl-rep-cli`。项目来源和 OpenBW 授权状态见仓库的 [ATTRIBUTION.md](../../ATTRIBUTION.md)。解析器使用 [screp v1.13.9（Apache-2.0）](https://github.com/icza/screp/tree/v1.13.9)，重制版模拟器链接 [zlib](https://zlib.net/)。
