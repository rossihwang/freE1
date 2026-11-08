# freE1 — 搜狗 E1 的开源录音固件（应用层）

原厂 ZhiyinOS 之上替换 App，不刷机：48 kHz / 24-bit（可选 16-bit）立体声录音、1 小时分段、
暂停/继续、断电恢复、录音标记、耳机监听、浏览器导出与删除、WiFi 配置、深色 LVGL 界面。

仓库内的文档：

- [`docs/INSTALL.md`](docs/INSTALL.md) — 在 Windows / Linux 上安装与卸载（只需 adb 和两个文件）
- [`docs/TEST_PLAN.md`](docs/TEST_PLAN.md) — 各项测试的结果与复现方法
- [`docs/MIC_ARRAY_GEOMETRY.md`](docs/MIC_ARRAY_GEOMETRY.md) — 麦克风阵列几何实测（64 × 25 mm）
- [`docs/MARKS.md`](docs/MARKS.md) — 录音标记文件格式与导出到 DAW
- [`docs/BACKLOG.md`](docs/BACKLOG.md) — 未做的功能与已知限制

## 首次使用：配置设备地址

脚本不再把设备 IP 写死在仓库里，统一读仓库根目录的 `e1env`（已在 `.gitignore`）：

```bash
cp e1env.example e1env
vi e1env          # E1_HOST=root@<你的设备IP>
```

`e1env` 支持两个变量：`E1_HOST`（必填，`root@<IP>`）、`E1_KEY`（可选，ssh 私钥路径；
不写就依次找几处常见位置（项目外的 `.e1work/ssh/id_ed25519`、仓库内的同名路径、`~/.ssh/id_ed25519`）。
也可以临时用环境变量覆盖：`E1_HOST=root@10.0.0.5 ./device-app.sh status`。

## 构建前置

仓库不含第三方源码，首次构建前取一次：

```bash
./scripts/fetch-lvgl.sh      # LVGL 9.2.2
./scripts/fetch-alsa.sh      # libasound（回放用，从设备拉取）
```

硬件测试程序（`hwtest/`）用同一套工具链，构建方法见 [`hwtest/README.md`](hwtest/README.md)。

> 设备端回放改为**进程内 ALSA**（不再 fork aplay），因此固件从全静态变成
> 动态链接 `libasound.so.2`（设备自带，原厂 aplay 也依赖它）。

## 快速开始（PC 预览，不需要设备）

```bash
./run_pc.sh          # 编译 + 生成演示录音 + 开窗口 + HTTP :8080
ls shots/            # 静态截图（由 freE1 shots 生成）
./build_pc/freE1 swipetest     # 手势自测（翻页/复位）
```

## 设备（E1 本体）

需要一份 aarch64 交叉工具链，**版本要和设备上的 glibc 匹配**（构建脚本只用到 `aarch64-linux-gcc`，
工具链本身不在本仓库）：

| 项 | 值 |
|---|---|
| 工具链 | Bootlin `aarch64--glibc--stable-2018.02-2` |
| 编译器 | `aarch64-linux-gcc` **6.4.0**（Buildroot 2018.02） |
| libc | **glibc 2.26**（与原厂 Buildroot 2018.02 一致） |
| 下载 | `https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--glibc--stable-2018.02-2.tar.bz2` |

版本是照设备上运行的二进制定的，不是随便挑的：换更新的工具链会引入设备上不存在的
glibc 符号，程序在设备上直接 `symbol not found`。

```bash
export PATH=<你的交叉工具链>/bin:$PATH
make -f Makefile.e1ui -j8       # 产物 build_e1ui/freE1

./device-app.sh enable          # 装机 + 切到 freE1（带 3 次崩溃回退）
./device-app.sh status
./device-app.sh disable         # 恢复原厂 App
```

## 交互

| 操作 | 行为 |
|---|---|
| 左右拖（或 ←/→） | 顶层翻页：Record ⇄ Files ⇄ Settings |
| 设置子页 `<` | 返回上一层（WiFi 等子页压栈） |
| 点 chip | DIRECTIONAL ⇄ OMNI |
| 点中间按钮 | 开始录音 → 暂停 → 继续（按钮文字随之变 REC/PAUSE/RESUME） |
| 点音量区 | 峰值保持复位 |
| 点文件行 | 播放；再点同一行暂停 / 继续 |
| 长按正在播放的行 | 快进（约 4×），松手恢复常速 |
| 长按其它行 | 删除（二次确认） |
| 音量键 +/− | 回放音量 ±5%，屏幕显示 `VOL nn%`（不影响录音电平与监听） |
| 灭屏时长按 REC | 直接开始录音（短按只亮屏） |
| 物理键 MARK | 录音中打一个标记（不振动） |
| 物理键 STOP | 停止录音；没在录音则停止回放 |
| 物理键 REC/PAUSE | 开始录音 → 暂停 → 继续 |
| 长按中间按钮 | 停止录音（触摸也能停） |
| 电源键短按 | 熄屏 / 亮屏 |
| 电源键长按 2 s | 关机确认（录音中拒绝） |

## 录音

- PDM 8 通道 → 选路 → 48 kHz / 24-bit `S24_3LE`（可选 16-bit，体积减半）
- 暂停：继续读管道但不落盘、不计分段时长，接缝处 5 ms 淡入淡出
- **DIRECTIONAL**：ch6/ch7；**OMNI**：ch0–ch5 左右列混音
- 1 小时/段（实测第一段精确 3600.0 s，序号自动递增）
- 启动丢弃 PDM 400 ms 瞬态；DC 阻断 + AGC（目标 −24 dBFS，峰值感知，噪声门限）
- `session.json` 与 WAV 每秒 fsync（在独立线程做，不挡采集）；启动自动修复未闭合 WAV 头
- 录音中不振动（马达是结构声，会被录进去）
- 满盘 < 0.2 GB / 低电 < 5%（未充电）自动停录，状态栏变红提示
- **电平告警灯**：录音中写入电平 ≥ 0.7 FS 亮黄、≥ 0.9 FS 亮红（都是常亮；正常录音和待机不亮），点音量区复位熄灭

## 回放

点文件行即播放，**再点同一行暂停、第三次点继续**；播放中这一行底色压暗，
背景有一条绿色进度条按播放位置增长（没有单独的底部控制栏）。

- 点别的行：换曲
- 滑到别的页：停止播放
- 文件自然播完：停止并刷新列表
- 开始录音：自动停止播放

## 文件列表

- 按文件名里的序号排序（`REC_0154` → 154，倒序），**与时钟无关**：设备没有 RTC 电池，
  断电后时钟会退回 2019，按时间排会把新录音排到最底部
- 板上有 RTC（RK808 电源管理芯片内置，`/dev/rtc0`，内核 `hctosys=1`），**且断电后仍在走**
  （实测断电 9 秒后开机，内核从 RTC 读回的时间正好接上）；但出厂后从没人写过它，
  所以以前每次开机都是 2019。固件现在**开机和每 10 分钟校一次**（差超过 60 s 才写，写 UTC），
  系统时间不可信（< 2024）时不动 RTC —— 这样即使没有网络，断电重启时间也是对的
- 每分钟把当前时间写进 `last_time`（只往前写），开机发现时钟明显偏早就用它顶回去，
  所以文件名即使在没有网络的情况下也能保持单调
- 每行显示时长、大小、采样率/位深、是否有标记

## HTTP 导出

**开机默认关闭**（不监听任何端口）。需要时在 `Settings → HTTP server` 点开，
显示为 `<设备IP>:8080` 后，浏览器访问即可：列表、Range 下载（可边下边听）、
**多选删除**（勾选/全选 → 删除选中，删前弹确认，会连同 `.marks` 一起删）。
删除接口只接受 POST 且要求页面渲染的一次性令牌，GET 会被拒绝（405）。
再点一下关闭。每次开机会回到关闭状态。

> HTTP 没有认证，只在可信网络里开。

## 目录

```
src/
  main.c        CLI: record / recover / ui / ui-dev / shots / wifi / swipetest
  rec.c         录音主循环（分段、AGC、瞬态丢弃、恢复）
  wav.c         S24_3LE 写入 + 头修复
  session.c     session.json
  audio_*.c     模拟源 / arecord 子进程
  engine.c      录音线程（UI 读状态）
  ui.c          LVGL 界面（Record/Files/Settings + 子页栈）
  wifi*.c       wpa_cli 封装 + WiFi 页
  http.c        极简 HTTP（列表/下载/多选删除）
  marks.c       录音标记 sidecar（.marks）
  monitor.c     耳机监听（进程内 ALSA 播放）
  audio_play.c  文件回放（进程内 ALSA + 暂停/跳转）
  hal_*.c       fb0 显示 / MT-B 触摸 / 物理键
Makefile.pc / Makefile.e1 / Makefile.e1ui
common/hal_sysfs.c   共享的 LED/马达/背光/耳机/电池/CPU 辅助
scripts/        启动脚本、部署、几何分析（geo-tdoa.py）、fb2png
tools/          marks-export.py（标记导出到 DAW）
```

## 已验证

- 10 h 连续采集：**13,824,000,000 B = 36000 s × 384000 B/s，精确**，0 xrun
- 断电恢复：`kill -9` 后头部 `data` 恰好 = 文件大小 − 44
- 设备端 UI：fb0 渲染、MT-B 触摸、物理键、WiFi 扫描/键盘、HTTP（截图见 `shots/`）
- AGC：−66 dBFS → −34 dBFS（峰值 −14，不削波）；正常语音 RMS −24 dBFS 稳定，无削顶
- 监听：0 欠载/0 丢块（68 s），延迟约 85–130 ms
- 分段轮转：第一段精确 3600.0 s，0 xrun
- 重启/休眠唤醒：`PM: suspend entry/exit` 正常，唤醒后 WiFi 自恢复
- WiFi 错密码：约 25 s 报失败并回退到原网络，已保存网络不被删除
- 低电/满盘保护：两条都实测触发并停录

## 电源策略（实测）

| 状态 | 电流 | 满电续航 |
|---|---|---|
| 录音中 | −180 mA | ≈ 11.7 h |
| 待机亮屏 | −141 mA | ≈ 15 h |
| 待机熄屏 | −87 mA | ≈ 24 h |
| 深度休眠（`mem`） | −18 mA | ≈ 5 天 |

空闲 60 s 熄屏 → 再 180 s 自动休眠；**只能电源键唤醒**（内核无 wakeup 源；唤醒后 1.5 s 内的按键会被吞掉，避免重复投递造成的闪屏）；
录音中允许熄屏但**禁止休眠**。**不做开机图**：进程起来直接显示主界面（原厂 u-boot/内核阶段的黑屏无法覆盖，见限制清单 L14）。

## 未验证 / 已知限制

- 44.1 kHz 不可用：PDM 在 44.1 kHz 下 ch6/ch7 恒为 0（见 `docs/BACKLOG.md`）
- ch6/ch7 不是可用的独立阵元（对方向不敏感），暂不用于 DOA/波束成形
- Omni 时延对齐：麦位已测出（64 × 25 mm），对齐本身还没做
- 电池续航曲线未做长测；关机只做了守卫测试
- 无 VAD、无多机（v2）
- MARK 标记的回放跳转未做（只打点 + 显示有没有）
