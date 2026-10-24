# Octo Radio 1.0

[English](README.en.md) | **简体中文**

Octo Radio 是一台基于 ESP32-C3 的联网收音机，使用像素风章鱼动画作为主界面。
设备支持城市定位与搜索、频道切换、FM 信息显示、配置持久化和三键操作。

![Octo Radio 主界面](assets/screenshots/main.png)

## 功能

- 自动 IP 定位、内置城市和全拼/首字母搜索；当前支持 203 个有可播放频道的城市。
- Radio Browser 电台目录，按已知 FM 频率排序并合并重复流、别名和同名频道。
- 离线 FM 表、Radio Browser `tags` 和在线频道元数据逐级补充频率。
- 城市、频道和音量持久化；启动时先显示上次配置，再刷新在线目录。
- MP3 解码、断流重连、8 KB 音频抖动缓冲、实时电平和音量控制。
- 设备端 Wi-Fi 配网，也可通过 `http://192.168.4.1` 配置。
- 亮度、自动熄屏、定时关机和已保存网络管理。

本项目只播放 MP3 网络流，不包含 HLS/AAC 播放器。需要登录、地区限制或已下线的
上游频道仍可能无法播放；本项目不托管任何电台内容。

## 硬件

当前 BSP 面向以下参考板，不是通用 ESP32-C3 固件：

| 部件 | 配置 |
| --- | --- |
| 主控 | ESP32-C3，160 MHz，8 MB Flash，无 PSRAM |
| 屏幕 | ST7789P3 兼容 240×320 TFT，SPI |
| 音频 | ES8311，I2S |
| 电量计 | CW2017，I2C |
| 按键 | 三个按键，共用 ADC |
| 控制台 | USB Serial/JTAG |

引脚定义集中在
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h)。
移植到其他板卡时需要核对屏幕、音频、I2C 和按键接口。

## 快速开始

需要 Git、Python 3.9+ 和
[ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/)。
首次构建需要联网，以按 `dependencies.lock` 下载组件。
将 `PORT` 指向本机串口。

```bash
git clone https://github.com/lison-xulicong/octo-radio.git
cd octo-radio

export IDF_PATH=/path/to/esp-idf
. "$IDF_PATH/export.sh"

./tools/build_variant.sh full
export PORT=/dev/ttyACM0
idf.py -B build-full -p "$PORT" flash monitor
```

macOS 串口通常为 `/dev/cu.usbmodem*`，Linux 通常为 `/dev/ttyACM0` 或
`/dev/ttyUSB0`。Linux 用户需要串口设备权限。

构建变体：

| 变体 | 说明 |
| --- | --- |
| `full` | 完整章鱼动画，默认 |
| `lite` | 减少手臂过度绘制 |
| `off` | 仅显示水面 |

```bash
./tools/build_variant.sh lite
./tools/build_variant.sh off
```

输出位于 `build-<variant>/Octo-Radio.bin`。构建目录、ELF、map、`sdkconfig`
和本地生成配置均被 Git 忽略。

## 首次启动

1. 烧写固件并复位。
2. 未保存 Wi-Fi 时，设备启动 `OCTO Radio` 热点。
3. 在设备上选择 2.4 GHz 网络，或连接热点后访问 `http://192.168.4.1`。
4. 连接成功后加载保存的城市和频道，并开始播放。

自动定位使用公网 IP，精度为城市级，受运营商出口、CGNAT 和 VPN 影响。定位不准时
可使用手动选择或搜索。除非需要清除设置，否则不要擦除 NVS。

## 按键

| 页面 | 上 / 下 | 短按确定 | 长按确定 |
| --- | --- | --- | --- |
| 主界面 | 上一台 / 下一台 | 播放 / 暂停 | 进入设置 |
| 设置 | 选择项目 | 进入或修改 | 返回 |
| 键盘 | 在按键间移动 | 输入 | 执行对应功能 |
| 键盘，长按上 / 下 | 沿当前列上下滚动一行 | — | — |

主界面长按上 / 下调节音量。

## 城市与 FM 数据

[`data/radio_cities.csv`](data/radio_cities.csv) 保存 337 个行政区。
[`data/radio_city_availability.csv`](data/radio_city_availability.csv) 记录
2026-10-24 的 Radio Browser 可用性审计：

| 项目 | 数量 |
| --- | ---: |
| 行政区源数据 | 337 |
| 当前可搜索 | 203 |
| 无可播放 MP3，已排除 | 134 |

运行时只嵌入有可播放 MP3 的城市。搜索不到某个城市通常表示上游当前没有符合条件的
频道，而不是键盘故障。刷新审计并重新生成固件表：

```bash
python3 tools/build_city_table.py --refresh
python3 tools/build_frequency_table.py
IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh
./tools/build_variant.sh full
```

FM 数据维护在 [`data/fm_frequencies.csv`](data/fm_frequencies.csv)，当前包含
87 条记录、25 个城市；字段和核验规则见 [`data/README.md`](data/README.md)。
设备只读取生成的紧凑 Flash 表，不把 CSV 载入 RAM。无法确认地面频率的纯网络电台
保留刻度但隐藏指针，不会编造 FM 数值。

Radio Browser、蜻蜓元数据及其他上游服务可能变化或暂时不可用。固件会回退到内置表、
频道名称和上次成功目录。

## 测试

主机测试不要求连接设备：

```bash
IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh
```

测试覆盖城市表、频率解析、目录解析、MP3 同步、调谐显示、字库和章鱼渲染。
如要使用保存的目录响应，可通过 `CATALOG_SAMPLES` 指定 fixture 目录：

```bash
CATALOG_SAMPLES=/path/to/fixtures \
IDF_PATH=/path/to/esp-idf \
./tools/tests/run_tests.sh
```

## 项目结构

```text
.
├── assets/screenshots/        README 截图
├── components/bsp/            屏幕、音频、按键、电池和 I2C
├── components/esp-wifi-connect Wi-Fi 连接与网页配网
├── data/                      城市、FM 和审计源数据
├── main/                      固件、播放器、目录、UI 和字库
├── tools/
│   ├── build_variant.sh       Full/Lite/Off 构建
│   ├── build_city_table.py    城市可用性审计
│   ├── build_frequency_table.py
│   ├── build_radio_font.py
│   ├── octopus/               动画原型与资源生成
│   └── tests/                 主机测试
├── dependencies.lock          固定的 ESP-IDF 组件版本
├── partitions.csv
└── sdkconfig.defaults
```

字库和章鱼资源已经提交，普通构建不需要 Node.js。只有重新生成字体或动画资源时，
才需要安装 Node.js；具体命令见相应脚本的 `--help` 和
[`tools/octopus`](tools/octopus)。

## 贡献

- 提交改动前运行 `IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh`。
- 更新城市数据后运行 `tools/build_city_table.py --refresh`，并提交生成的
  `main/radio_cities.h`。
- 更新 FM 数据后运行 `tools/build_frequency_table.py`，并提交生成的
  `main/radio_frequency_data.h`。
- 不要提交 `build-*`、`managed_components`、ELF、map 或本地 `sdkconfig`。
- 广播流可用性属于外部服务，问题报告请附上城市、频道名和脱敏后的日志。

## 来源与许可证

Octo Radio 是基于 [LEO Radio](https://github.com/leo0183/leo-radio) 的独立衍生
项目。原项目与本项目均使用 MIT 许可证，版权声明保留在 [`LICENSE`](LICENSE)。

Noto Sans SC 使用 SIL Open Font License 1.1；LVGL、Espressif 组件和
`esp-wifi-connect` 等依赖的许可见
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) 与 [`LICENSES`](LICENSES)。
电台目录来自公开的 [Radio Browser](https://www.radio-browser.info/) API。
