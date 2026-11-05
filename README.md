# freE1 — 搜狗 E1 的开源录音固件（应用层）

原厂 ZhiyinOS 之上替换 App，不刷机：48 kHz / 24-bit 立体声录音、1 小时分段、断电恢复、浏览器导出、WiFi 配置、深色 LVGL 界面。

> 设计决策见 [`../freE1_app_design_v2.md`](../freE1_app_design_v2.md)，
> 系统限制见 [`../E1_ZhiyinOS限制清单.md`](../E1_ZhiyinOS限制清单.md)，
> 实测记录见 [`../verify/FINDINGS.md`](../verify/FINDINGS.md)。

## 构建前置

仓库不含第三方源码（LVGL 9.2.2），首次构建前先取一次：

```bash
./scripts/fetch-lvgl.sh
```

## 快速开始（PC 预览，不需要设备）

```bash
./run_pc.sh          # 编译 + 生成演示录音 + 开窗口 + HTTP :8080
ls shots/            # 静态截图（含设备真机截图 08/09）
./build_pc/freE1 swipetest     # 手势自测（翻页/复位）
```

## 设备（E1 本体）

```bash
source ../cross/env.sh
make -f Makefile.e1ui -j8          # 静态 aarch64
../freE1/device-app.sh enable      # 装机 + 切到 freE1（带 3 次崩溃回退）
../freE1/device-app.sh status
../freE1/device-app.sh disable     # 恢复原厂 App
```

## 交互

| 操作 | 行为 |
|---|---|
| 左右拖（或 ←/→） | 顶层翻页：Record ⇄ Files ⇄ Settings |
| 设置子页 `<` | 返回上一层（WiFi 等子页压栈） |
| 点 chip | DIRECTIONAL ⇄ OMNI |
| 点中间按钮 | 开始 / 停止录音 |
| 点音量区 | 峰值保持复位 |
| 长按文件行 | 删除（二次确认） |
| 物理键 MARK / STOP | 上一页 / 下一页 |
| 物理键 REC | 开始 / 停止 |
| 电源键长按 2 s | 关机确认（录音中拒绝） |

## 录音

- PDM 8 通道 → 选路 → 48 kHz / 24-bit `S24_3LE` 单产物
- **DIRECTIONAL**：ch6/ch7；**OMNI**：ch0–ch5 左右列混音
- 1 小时/段（`arecord` 在 2 GiB 也会自动切）
- 启动丢弃 PDM 400 ms 瞬态；DC 阻断 + AGC（目标 −24 dBFS，峰值感知，噪声门限）
- `session.json` 每秒落盘；启动自动修复未闭合 WAV 头
- 满盘 < 0.2 GB / 低电 < 5%（未充电）自动停录

## HTTP 导出

**开机默认关闭**（不监听任何端口）。需要时在 `Settings → HTTP server` 点开，
显示为 `192.168.3.89:8080` 后，浏览器访问即可（列表 + Range 下载，可边下边听）。
再点一下关闭。每次开机会回到关闭状态。

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
  http.c        极简 HTTP（Range）
  hal_*.c       fb0 显示 / MT-B 触摸 / 物理键
Makefile.pc / Makefile.e1 / Makefile.e1ui
scripts/        launcher、fb2png
```

## 已验证

- 10 h 连续采集：**13,824,000,000 B = 36000 s × 384000 B/s，精确**，0 xrun
- 断电恢复：`kill -9` 后头部 `data` 恰好 = 文件大小 − 44
- 设备端 UI：fb0 渲染、MT-B 触摸、物理键、WiFi 扫描/键盘、HTTP（真机截图见 `shots/08-*`、`09-*`）
- AGC：−66 dBFS → −34 dBFS（峰值 −14，不削波）

## 电源策略（实测）

| 状态 | 电流 | 满电续航 |
|---|---|---|
| 录音中 | −180 mA | ≈ 11.7 h |
| 待机亮屏 | −141 mA | ≈ 15 h |
| 待机熄屏 | −87 mA | ≈ 24 h |
| 深度休眠（`mem`） | −18 mA | ≈ 5 天 |

空闲 60 s 熄屏 → 再 180 s 自动休眠；**只能电源键唤醒**（内核无 wakeup 源；唤醒后 1.5 s 内的按键会被吞掉，避免重复投递造成的闪屏）；
录音中禁止熄屏与休眠。**不做开机图**：进程起来直接显示主界面（原厂 u-boot/内核阶段的黑屏无法覆盖，见限制清单 L14）。

## 未验证 / 已知限制

- WiFi **连接**新网络未端到端测（会断 SSH）
- 关机只做了守卫测试，未真关过机
- 电池续航未测（一直插电）
- 无播放功能（v1 不做）
- Omni 未做时延对齐（需先量麦几何）
- 无 VAD、无多机（v2）
