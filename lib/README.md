# 第三方静态库

Linux 与 Windows x86_64 静态归档分别位于 `linux/`、`windows/`。
依赖包括 ImGui、libvterm、SQLite、libssh2、mbedTLS（含 everest/p256m）、zlib 和 SDL2。
对应平台头文件位于 `include/vendor/<平台>/`，ImGui 位于 `include/imgui/`。
各平台 `SHA256SUMS` 用于校验归档；第三方许可证保存在 `third_party/licenses/`。
仅保留静态库、必要头文件和应用需要编译的后端，不需要保留依赖构建源码。

## ImGui

版本：Dear ImGui 1.91.9b-docking。
许可证：MIT，见 `third_party/licenses/ImGui.txt`。

| 路径 | Zig 目标 |
| --- | --- |
| `linux/libimgui.a` | `x86_64-linux-gnu.2.35` |
| `windows/libimgui.a` | `x86_64-windows-gnu` |

由 Zig `0.17.0-dev.1683+5ceec001b`、`-std=c++11 -O2` 编译
`imgui.cpp`、`imgui_draw.cpp`、`imgui_tables.cpp`、`imgui_widgets.cpp`。
配套头文件与应用需要编译的 SDL/OpenGL 后端保存在 `include/imgui/`。
生成静态库的上游实现源码、下载包和临时对象文件已删除。
清理项目 `build/`、`.zig-cache/`、`zig-out/` 时应保留 `lib/` 和 `include/imgui/`。

SHA256：

```text
7c5d4de950dd47794995d7a60b0ea7747003423670b735a2802021f6c6d6cfd2  linux/libimgui.a
4912e17394ee448680ec1bd87c12b9c17fa7db7104448767af787a357eae02f8  windows/libimgui.a
```
