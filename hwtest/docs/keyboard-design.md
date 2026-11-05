# 原厂 App 键盘设计提取

从 `SogouE1`（ZhiyinOS 1.2.3）二进制中提取，方法：

1. 明文段里直接扫 QML（数字键盘、符号键盘就是明文）
2. 其余 QML 在 qrc 资源里被 **zlib 压缩**，扫 `78 9c` 流解压，恢复出 **103 个 QML 文件**
   （`.e1work/qml/`，关键文件已归档到 `hwtest/docs/qml/`）
3. 输入法引擎符号来自二进制符号表（`_sgime_core_pinyin_*` 等）

> 屏是 320×320 的 1.54" 屏，全部尺寸都按 320 宽设计。

## 1. 组件清单

| QML/组件 | 用途 | 来源 |
|---|---|---|
| `NumberKeyButton` | 数字键层（1-9 / 退格 / 0 / 确定） | `number_keypad.qml`（明文） |
| `NumberKeyButton`（变体） | 数字 + `.` + `:`，用于时间/地址输入 | `number_dot_keypad.qml` |
| `OtherKeyButton` | 符号层（38 个符号，可滚动） | `symbol_keyboard.qml` |
| `EnterKeyButton` | 第三层键位（WiFi 密码页使用） | 与上同类网格 |
| `CodeKeyButton` | 验证码/密码数字输入（带 ○ 占位、自动提交） | `password_page.qml` / `verify_code_page.qml` |
| `T9 键盘` | 九键字母（abc/def/ghi… + 动态字母键行） | `t9_keyboard.qml` |
| `KeyButton` / `ClickKeyButton` | 对话框里的圆形按键（80×80，支持长按） | 明文 QML |
| `KeyButtonControl` | 顶栏图标按钮（返回/清除） | 明文 QML |

## 2. 数字键盘（验证码 / 密码 / 激活码）

```
[ 1 ][ 2 ][ 3 ]
[ 4 ][ 5 ][ 6 ]
[ 7 ][ 8 ][ 9 ]
[ ⌫ ][ 0 ][ 确定 ]
```

- 网格：3 列 × 4 行，`cellWidth = width/3`，`cellHeight = height/4`
- 底色 `#000000`，1 px 边框 `#2F313F`，悬停 `#1A1A2C`
- 文字 `#E8E8E9`，**40 px**（"确定" 24 px），`Font.Medium`
- 退格：`qrc:/sogou/images/btn_backspace(_dis).png`
- "确定" 仅在输入合法（`ok`）时可点；颜色动画 **3000 ms**
- 信号：`click(string key)` / `enter` / `remove`

## 3. 符号键盘

- 3 列网格，38 个符号（`[ ] { } # % ^ * + = _ \ | ~ < > € $ ￥ · - / : ; ( ) & @ " . , ? ! ' ，。、`）
- 可滚动（`ScrollBar.vertical`），文字 26 px，样式同上

## 4. WiFi 密码页（最完整的一套）

布局（320×320）：

```
┌──────────────────────────────┐
│ 60 px 顶栏: ←    输入文本(28px)      ✕ │
├───────────────────────┬──────┤
│                       │ ⌫    │
│   键区 240 × 261      │ 空格  │  右列 80 宽 × 4 行
│   （三层切换）         │ 123/abc│  每行 ~65 px
│                       │ 连接  │
└───────────────────────┴──────┘
```

- 顶栏 60 px：返回箭头 `btn_last.png`(16×36)、输入回显（28 px，居中、`ElideMiddle`，空时显示"输入<SSID>的密码"）、清除 `btn_clear.png`(36×36)
- 键区 240 px 宽、261 px 高；右列 80 px 宽 4 行：
  1. **⌫ 退格**（有输入才亮）
  2. **空格**（`btn_space.png`）
  3. **层切换**（`btn_language` / `btn_language_abc` / `btn_language_123`，`keyMode` 0→1→2 循环）
  4. **"连接"**：长度 < 8 灰、≥ 8 变橙 `#FF6830`
- 键区颜色：`#0d0d12` 底、`#2F313F` 边框、`#E8E8E9` 字、26 px；动画 200 ms
- 输入方式：每个键 `onClick: value += key`，退格 `substring(0, len-1)`

## 5. T9 九键字母键盘

```
[动态键1][动态键2][动态键3]      ← 1/4 高，由上次点击的字母组填充
┌────┬────┬────┐
│  ⇧ │abc │def │
├────┼────┼────┤
│ghi │jkl │mno │
├────┼────┼────┤
│pqrs│tuv │wxyz│
└────┴────┴────┘
```

- 顶部行 3 个键：点击下面字母组后，把该组字母（最多 4 个）填进这 3 个位置；`lowMode` 切换大小写
- 左上格是 `⇧`（图标 `btn_top.png`），点击切大小写
- 字母组：`abc / def / ghi / jkl / mno / pqrs / tuv / wxyz`
- 颜色：顶部行 `#28283D`、分隔线 `#3d415E`；网格 `#0d0d12`、悬停 `#11131f`、边框 `#2F313F`、字 26 px
- 信号：`click(string key)`

## 6. 对话框圆形按键

- 80×80，`radius = height * 0.5`
- 信号：`clickKeyButton` / `pressAndHold`（长按连发）
- 悬停变色，3 s 颜色动画

## 7. 输入法引擎（字母/候选词的真实来源）

二进制里静态链接了搜狗输入法内核，符号与模型名包括：

- `_sgime_core_pinyin_9n_newDict`（九键）、26 键纠错、笔划、五笔、英文
- 网络名：`拼音网络` / `英文拼音网络` / `拼音串网络` / `九键纠错` / `26键纠错`
- 输入状态：`LatinCore_ComposingOnKey` / `ComposingOnBackspace`
- `InputKeyboardType`（引擎区分键盘类型）
- 按键音：`SetIsPlayKeyTone` / `GetIsPlayKeyTone`

所以"字母键盘 + 候选栏"不是 QML，而是这套 C++ 引擎提供的；QML 里只有数字/符号/T9 三套键帽。

## 8. 物理按键（补充）

- 机身 6 键：`KEY_1..KEY_5`（adc-keys，event3）+ `KEY_6`（PMIC，event1）
- 映射：KEY_1=MARK、KEY_2=STOP、KEY_3=REC、KEY_4=VOL+、KEY_5=VOL−、KEY_6=POWER
- QML 里能看到 `Keys.onPressed` + `Config.Key_Stop` 的物理键处理痕迹（音乐页被注释掉），长按逻辑在 C++

## 9. 未提取到的部分

- **26 键 QWERTY 的键帽布局**：引擎支持 26 键纠错，但提取到的 QML 里没有 QWERTY 网格，
  大概率由输入法引擎自绘或位于未解压的资源块中
- T9 键盘（`t9_keyboard.qml`）在提取到的页面里没有被实例化，说明它的宿主页面
  （可能是搜索/命名页）不在已恢复的 103 个 QML 里
- 圆角/图片资源本身在 qrc 中，未导出（只拿到了文件名）

## 复现

```bash
# 已归档的 QML：
hwtest/docs/qml/t9_keyboard.qml
hwtest/docs/qml/wifi_password_page.qml
hwtest/docs/qml/password_page.qml
hwtest/docs/qml/verify_code_page.qml
hwtest/docs/qml/number_keypad.qml
hwtest/docs/qml/number_dot_keypad.qml
hwtest/docs/qml/symbol_keyboard.qml
```
