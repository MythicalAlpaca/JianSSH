# JianSSH

**简体中文** | [English](README.en.md)

基于 C++20、Zig、Dear ImGui、SDL2、libssh2、mbedTLS、libvterm 与 SQLite 的轻量级 SSH 桌面客户端。

> **项目状态：桌面版。** Linux x86_64 与 Windows x86_64 均已通过实机运行验证。Windows 上凭据库、SSH Agent、本地 chmod 和系统主题探测仍不可用或受限，详见下文平台支持与安全模型。功能范围与数据格式以本版本为准，后续版本可能继续调整。

## 功能特性

### 设备与会话

- 设备新增、编辑、删除、分组和搜索；设备资料存入 SQLite（WAL 模式），不包含密码字段。
- 快速连接与多会话标签页；连接方式可选 SSH 或 Windows/Linux 串口。串口可刷新列表或手动输入设备路径，默认 115200 8N1、无流控；新增设备可保存串口配置，快速连接不会自动保存。
- 串口会话复用终端，不提供 SSH 文件传输、远端监控、认证和主机密钥验证。断开时未发送数据会被丢弃并提示。
- 打开串口可能改变 DTR/RTS，部分设备可能复位；Linux 用户需自行确认设备访问权限（通常由 `dialout` 组或设备 ACL 控制）。不自动修改权限，不自动发送登录命令。
- 支持密码、公钥（私钥旁可放置同名 `.pub`）和 SSH Agent（Linux）认证。私钥通过文件选择器选取，选择及提交时检查文件可读性和格式；Linux 文件选择器依赖 `zenity`。旧版 keyboard-interactive 设备须编辑并保存其他认证方式后才能连接。
- 私钥格式检查仅为初筛，不验证私钥口令或保证算法可用于连接；具体格式和口令由 SSH 认证时验证。
- 首次连接展示 SHA256 主机密钥指纹并要求确认；信任记录成功写入后才继续认证；已知密钥不匹配时直接拒绝连接。
- 主机密钥按应用内部“原始公钥 + 主机/端口”保存，不读取或改写 OpenSSH 的 `known_hosts`。

### 终端

- 后台单 owner 非阻塞 SSH IO，支持交互 PTY、窗口尺寸变化和有界输出背压。
- 基于 libvterm 的终端渲染，回滚上限 2000 行。
- 鼠标滚轮上下浏览回滚历史，Shift+PageUp/PageDown 翻页；输入时回到实时屏幕，也可通过右键菜单“回到实时输出”返回。在启用鼠标上报的 vim/tmux 等程序中，滚轮仍交给远端，按住 Shift 可浏览本地历史。
- 输入、粘贴与 PTY resize 不阻塞界面线程；粘贴先预览确认并限制大小。
- 断线后可在原标签点击「重试连接」，无需重填连接地址；不会自动重连，也不会重放未发送的输入。
- SSH 密码被拒绝时，可在同一连接内重新输入。对端停止接受密码或断开连接后不再提示。
- 双击设备列表中的 SSH 名称或地址，直接在新标签连接该设备。双击已连接的 SSH 标签名称（或 IP）、绿色「已连接」区域或底部状态栏，可为该目标打开独立标签，原会话不受影响；单击只选择。新连接仍验证主机密钥；未记忆口令时需重新输入。串口支持断线重试，不重复打开已占用的端口。

### 文件传输

- SFTP 本地／远端双栏各自保留目录和筛选状态，支持主目录、上一级、刷新和直接输入路径；可按名称、大小、修改时间排序，按名称筛选并切换隐藏项。窄窗口下两栏垂直排列。
- `Ctrl`（或 `⌘`）点选、`Shift` 连选或全选后，批量上传／下载普通文件；也可把单个文件拖到另一栏或该栏内的目录。右键菜单提供复制完整路径、重命名、删除文件和修改权限，工具栏可创建目录。
- 在「文件传输 → 展开队列」中，可按任务暂停、恢复或取消上传和下载。传输中点击「暂停」后，当前数据块可能先传完；显示「已暂停」后进度保持不变。点击「恢复」从该位置继续，不会重传整个文件。
- 同一会话一次只传一个文件。暂停正在传输的文件会保留其位置，该会话后续的文件传输、远端目录刷新和监控采样需等它恢复或取消；尚未提交的暂停任务则会被跳过。SSH 终端和其他会话仍可使用。队列最多保留 256 项，可清除已结束记录。
- 推送（上传）或获取（下载）遇到同名目标时，不会静默覆盖，而是弹出确认窗口提供「另存为」（自动建议递增编号名称并可编辑）、「覆盖」（临时文件完整校验后原子替换普通文件，远端需服务器支持 `posix-rename`）和「跳过／取消」选项，多文件批量传输还支持一键应用到其余冲突项；暂不提供递归传输／删除、目录删除或同一会话并发传输。暂停状态仅在当前连接有效；断线、关闭会话或退出应用后，需要重新发起传输。
- 传输临时文件使用 `0600` 权限，失败时不会通过删除目标文件来“回退”。

### 远端监控

- 连接成功后自动开启远端 Linux 采样：通过 SSH exec 读取 `/proc`、`df` 等数据。
- CPU、内存、磁盘和网络的最新值显示在底部资源条，不展示模拟数据。
- 采样失败按退避间隔重试；断线或停止监控后只保留历史数据并标注过期。

### 连接口令存储

- 记忆策略提供“从不记忆 / 仅本次运行 / 加密储存连接口令”，不再使用主密码库。
- 选择记忆策略后，只在认证成功后保存密码或私钥口令，OTP 不保存；口令输入可切换明文/掩码显示。
- 快速连接可勾选“保存到工作空间设备”；不勾选则不新增设备，记忆口令需关联已保存设备。
- 使用随机本机密钥和 XChaCha20-Poly1305 加密记录，目录权限 `0700`，密钥、数据和锁文件为 `0600`，独占锁避免多实例并发写入。
- 旧主密码库不会自动迁移或删除，请重新输入连接口令并选择新的记忆策略。
- Windows 目前没有凭据库后端，口令记忆策略均不可用，每次连接需重新输入口令；密码和私钥口令不会写入磁盘。

### 界面

- 内嵌中文与比例字体，macOS 风格的中性灰材质、系统蓝强调色与圆角控件；支持跟随系统、深色和浅色主题。
- 入场、悬停、选中、模态与页面切换过渡带有限时长；页面切换采用快出慢入的分段缓动，省电模式默认开启，空闲时按需绘制。
- 支持 `--reduced-motion` 和界面内的“减少动态效果”选项。
- 有会话时使用紧凑布局，优先为终端和 SFTP 留出空间。窄窗口通过会话栏的「设备」按钮打开设备导航，SFTP 传输队列默认折叠。

## 平台支持

| 目标 | 构建 | 运行验证 | 说明 |
| --- | --- | --- | --- |
| Linux x86_64 | ✅ | ✅ 实机验证 | 主力目标，要求 glibc ≥ 2.35；图形栈为 X11/Wayland + OpenGL3 |
| Windows x86_64 | ✅ | ✅ 实机验证 | GUI 子系统，单 EXE；凭据库、SSH Agent、chmod 和系统主题探测不可用或受限 |

Windows 目标使用依赖根下预编译好的静态归档，不会自动下载依赖；缺少归档时构建会明确报错。除 UCRT/系统 DLL 外不额外依赖 `SDL2.dll`。

## 快速开始

### 构建依赖

- Zig `0.17.0-dev.1683+5ceec001b`（必须使用该固定版本）
- Python 3、CMake、make
- Linux 构建还需要 `apt-get`、`dpkg-deb`（仅用于下载并解包图形栈开发包，不会安装到系统）
- 网络访问依赖源码归档与图形栈开发包

### 准备依赖

两平台静态库分别保存在 `lib/linux/`、`lib/windows/`；配套头文件位于 `include/vendor/linux/`、`include/vendor/windows/`，ImGui 头文件及 SDL/OpenGL 后端位于 `include/imgui/`。正常编译直接使用项目内的库和头文件，不需要下载依赖源码。以下命令仅用于重建其他依赖，`deps/` 为可清理的暂存目录：

```bash
python3 tools/fetch_deps.py      # 首次下载并校验固定版本源码到项目 deps/
./tools/build_core_deps.sh       # zlib / mbedTLS / libssh2 / libvterm / SQLite（ImGui 使用内置归档）
./tools/build_desktop_deps.sh    # SDL2 静态代码 + 桌面图形栈头文件
bash tools/build_core_deps.sh --windows
bash tools/build_windows_desktop_deps.sh
python3 tools/import_deps.py     # 将已安装静态库与头文件归入 lib/ 和 include/vendor/
```

两条脚本都会校验源码 SHA256，并只在独立的依赖根内构建，不修改系统环境。

### 构建与运行

```bash
./build.sh deps-check            # 检查显式静态归档是否齐全
./build.sh                       # 依次编译全部支持版本：Linux 和 Windows
./build.sh linux                 # Linux 桌面版 → zig-out/bin/JianSSH
./zig-out/bin/JianSSH

./build.sh run                   # 构建后直接运行
./build.sh dist-linux-x86_64     # 剥离符号的发布版 → zig-out/dist/JianSSH-linux-x86_64
./build.sh windows               # Windows x86_64 GUI 发布版 → zig-out/bin/JianSSH.exe
```

默认构建要求 Linux 和 Windows 两套静态依赖均已准备；任一目标失败时脚本立即报错退出。`./build.sh all` 等同默认行为，其他 Zig 构建选项可追加在目标参数之后。当前仅支持上述两个 x86_64 桌面目标，不包含 macOS。

最终链接只使用显式静态归档；运行期仍依赖系统的 glibc、X11/Wayland 与 OpenGL 驱动。字体和图标在构建时嵌入二进制，运行时不需要旁置资源文件。

### 版本号

版本在构建时生成并写入可执行程序。主窗口标题、「偏好设置」和 `--version` 都显示当前版本；Windows 还可在 EXE 的「属性 → 详细信息」中查看。

自动编号以 `1.00.00` 为基线，沿当前 Git 历史的第一父提交链计数，只计入改变 JianSSH 目录的提交（合并带来的改动算一次）。每 100 次向中间段进位，每 10,000 次向首段进位，例如 `1.00.99 → 1.01.00`。同一提交反复构建不会增加版本，未提交改动也不计数。运行 `python3 tools/generate_version.py` 可预览版本；无 Git 历史或浅克隆时默认使用 `1.00.00`，打包方可用 `JIANSSH_VERSION=1.00.09` 显式指定归档版本。

### 命令行参数

```text
JianSSH [--smoke-ms 毫秒] [--screenshot 文件.bmp] [--metrics 文件]
        [--data-dir 目录] [--reduced-motion]
        [--theme system|dark|light] [--window 宽x高]
```

| 参数 | 说明 |
| --- | --- |
| `--version` | 打印版本与目标平台后退出 |
| `--help` | 打印用法后退出 |
| `--window 宽x高` | 初始窗口尺寸，范围 `300x200` 到 `8192x8192` |
| `--theme system\|dark\|light` | 覆盖主题设置 |
| `--reduced-motion` | 减少动态效果 |
| `--data-dir 目录` | 使用指定目录下的 `jianssh.db`（凭据库位于同目录） |
| `--smoke-ms N` | 运行 N 毫秒后自动退出，范围 `0..3600000` |
| `--screenshot 文件.bmp` | 冒烟运行结束前保存 BMP 截图，需配合 `--smoke-ms` |
| `--metrics 文件` | 退出时写入帧数、等待次数、运行时长、主题等 JSON 摘要 |

### 环境变量

| 变量 | 说明 |
| --- | --- |
| `JIANSSH_TIMING=1` | 向 stderr 输出启动阶段耗时 |
| `JIANSSH_DEPS_ROOT` | 重建依赖时的暂存根目录，正常应用构建不需要 |
| `JIANSSH_CORE_PREFIX` | 覆盖核心库根目录，归档需位于其 `lib/linux/` 或 `lib/windows/` |
| `JIANSSH_SDL_PREFIX` | 覆盖 SDL2 库根目录，使用同样的平台子目录布局 |
| `JIANSSH_EXTRA_PREFIX` | 覆盖 Windows 附加库根目录，归档位于其 `lib/windows/` |
| `JIANSSH_PLATFORM=windows` | 在调用 `tools/build_project.sh` 时强制 Windows 目标 |

## 数据与文件位置

| 内容 | Linux | Windows |
| --- | --- | --- |
| 设备数据库 | `${XDG_CONFIG_HOME:-$HOME/.config}/JianSSH/jianssh.db` | `%APPDATA%\JianSSH\jianssh.db` |
| 加密凭据库 | 数据库同目录下的 `credentials.enc`（密钥为 `credentials.enc.key`） | 不可用（未实现 DPAPI） |

数据库的直属父目录应专用于本应用：程序会将其权限收紧到 `0700`，数据库、WAL、SHM 和凭据库文件为 `0600`。不要把 `--data-dir` 指向共享目录，也不要放入符号链接文件。

保存串口设备需要数据库结构升级到版本 3；现有 SSH 设备与主机信任记录会保留。旧版 JianSSH 不支持打开已升级的数据库，升级前如需保留回退能力，请备份整个应用数据目录。

## 项目结构

```text
.
├── build.zig                 # Zig 构建入口（调用 tools/build_project.sh）
├── build.sh                  # 便捷包装：./build.sh [all|linux|windows|run|dist-linux-x86_64|deps-check]
├── include/jianssh/          # 公共头文件
│   ├── app/                  # 会话、终端、凭据、本地文件等工作线程接口
│   ├── net/                  # SSH 会话与任务 API
│   ├── store/                # 设备与主机密钥存储
│   ├── ui/                   # 界面模型、主题、布局与动效
│   ├── metrics/              # 远端采样解析
│   └── util/                 # Zig/C++ 加密桥接
├── src/
│   ├── main.cpp              # SDL/ImGui 启动、渲染循环与命令行解析
│   ├── app/                  # 工作线程与控制器实现
│   ├── net/                  # libssh2 非阻塞会话实现
│   ├── store/                # SQLite 存储实现
│   ├── ui/                   # ImGui 界面实现
│   ├── metrics/              # 采样脚本与解析实现
│   └── zig/crypto.zig        # Argon2id、XChaCha20-Poly1305、安全擦除
├── assets/                   # 图标、字体及字体许可证
├── tools/                    # 依赖构建、资源嵌入、图标生成与产物校验脚本
├── third_party/licenses/     # 第三方许可证全文
├── NOTICE                    # 第三方组件署名
├── LICENSE                   # MIT
├── README.md                 # 简体中文
└── README.en.md              # English
```

## 架构说明

- **UI 主线程**只处理 SDL 事件、ImGui 绘制和模型快照，不直接进行文件、SQLite 或网络 IO。
- **HostService** 使用单一 owner 线程访问 SQLite，UI 通过有界命令队列提交操作并轮询不可变快照。
- **CredentialService** 使用独立工作线程执行本机密钥初始化、加解密和口令读写；退出时清除进程内秘密。
- **SshSession** 使用单独 IO owner 线程驱动 libssh2，输入、输出和事件队列均有容量上限；超出部分按背压策略处理。
- **LocalFiles** 与 **Collector** 分别负责本地文件操作和远端采样解析，避免阻塞界面。
- **按需渲染**：没有动画或新事件时降低帧率/等待事件；字体图集在窗口与 GL 上下文创建期间由后台线程生成，首帧上传 GPU 后释放 CPU 位图。
- **内存控制**：正文与终端复用同字号中文的图集区域；OpenGL 3.3 使用 R8 单通道字体纹理，避免 RGBA 扩展。构建时生成适配后的 ImGui 后端，不修改依赖源文件。终端回滚无损压缩重复的尾部单元，保留原始列数、颜色、样式和 2000 行上限。

## 安全模型与边界

- 加密口令采用 Linux 本机密钥模式；复制密文和密钥即可解密，不替代系统账户及磁盘安全。不提供整库回滚检测或 `mlock`，无法对已控制当前用户的攻击者保密。Windows 未实现 DPAPI 或等价的系统凭据后端，口令不会持久化。
- 主机密钥只保存在应用数据库；不支持导入 OpenSSH hashed `known_hosts`，也暂不提供密钥替换 UI。
- 终端支持鼠标拖动选择当前可见内容（含回滚历史），右键菜单可复制或粘贴；也可使用 Ctrl+Shift+C 复制、Ctrl+Shift+V 或 Shift+Insert 粘贴（粘贴需确认）；Ctrl+C 仍发送中断。搜索和全部兼容性回归尚未完成；vim、tmux 与 IME 仍待持续验证。
- SFTP 支持同连接内暂停和恢复，但不保存跨连接或跨重启的续传记录。
- 远端监控仅支持 Linux 远端；macOS/Windows 远端、多级跳板、代理和端口转发尚未实现。
- Windows 已通过实机运行验证；凭据库、SSH Agent、本地 chmod 在该目标不可用，系统主题探测不可用（系统主题模式回退为深色）。
- 项目没有自动更新、遥测或云同步；所有设备资料与凭据均保存在本机。

## 参与开发

- 提交改动前请先运行 `./build.sh deps-check` 和 `./build.sh`，确认显式依赖归档与链接路径未被破坏。
- 依赖版本固定在上文所列版本；升级依赖时同步更新 `tools/build_core_deps.sh`、`tools/build_desktop_deps.sh`、`NOTICE` 和 `third_party/licenses/`。
- 不要提交 `build/`、`.zig-cache/`、`zig-out/`、数据库或凭据库文件（已在 `.gitignore` 中忽略）。

## 第三方组件与许可

| 组件 | 版本 | 许可证 | 许可证文本 |
| --- | --- | --- | --- |
| libssh2 | 1.11.1 | BSD-3-Clause | [third_party/licenses/libssh2.txt](third_party/licenses/libssh2.txt) |
| mbedTLS | 3.6.4 | Apache-2.0 | [third_party/licenses/mbedTLS.txt](third_party/licenses/mbedTLS.txt) |
| zlib | 1.3.1 | zlib | [third_party/licenses/zlib.txt](third_party/licenses/zlib.txt) |
| SDL2 | 2.30.12 | zlib | [third_party/licenses/SDL2.txt](third_party/licenses/SDL2.txt) |
| Dear ImGui | 1.91.9b-docking | MIT | [third_party/licenses/ImGui.txt](third_party/licenses/ImGui.txt) |
| libvterm | 0.3.3 | MIT | [third_party/licenses/libvterm.txt](third_party/licenses/libvterm.txt) |
| SQLite | 3.47.2 | Public Domain | [sqlite.org/copyright.html](https://sqlite.org/copyright.html) |
| DejaVu Sans / DejaVu Sans Mono | — | Bitstream Vera / Public Domain | [assets/fonts/DejaVu-COPYRIGHT](assets/fonts/DejaVu-COPYRIGHT) |
| Droid Sans Fallback | — | Apache-2.0 | [assets/fonts/Droid-COPYRIGHT](assets/fonts/Droid-COPYRIGHT) |

运行时加载的系统 X11/Wayland/OpenGL 库不随本项目分发。完整署名与再分发注意事项见 [NOTICE](NOTICE)。

## 开源协议与引用

JianSSH 以 [MIT License](LICENSE) 发布，Copyright (c) 2026 JianSSH contributors。

在论文、报告或衍生项目中引用本项目时，可使用：

```bibtex
@misc{jianssh,
  title        = {JianSSH: A Lightweight SSH Desktop Client},
  author       = {{JianSSH contributors}},
  year         = {2026},
  version      = {1.0.0},
  note         = {C++20, Zig, Dear ImGui, SDL2, libssh2, libvterm, SQLite}
}
```

再分发二进制或修改版源码时，请保留 `LICENSE`、`NOTICE` 与 `third_party/licenses/` 中的许可证文本。
