# 内存回归验证

## 终端

使用与应用相同的 libvterm 静态库，开启断言构建测试：

```bash
zig c++ -std=c++20 -O2 -UNDEBUG -Iinclude \
  -Iinclude/vendor/linux \
  tools/test_terminal_memory.cpp src/app/terminal.cpp \
  lib/linux/libvterm.a \
  -o /tmp/opencode/test-terminal-memory
/usr/bin/time -v /tmp/opencode/test-terminal-memory
```

测试包含回滚行数、扩窗恢复、带颜色的空白尾部、中文宽字符、组合字符、
禁用回滚，以及 10 个终端各保留 2000 行、400 列的短输出负载。
该负载的最大 RSS 在本次 Linux 测试中从 322412 KiB 降为 24792 KiB。
这是短行场景，不代表填满整行的随机内容也有相同压缩率。

## 桌面

```bash
./build.sh deps-check
./build.sh
./zig-out/bin/JianSSH --smoke-ms 4000 \
  --data-dir /tmp/opencode/jianssh-memory-test
```

可在运行期间采样 `/proc/<pid>/status` 的 `VmRSS` 和 `VmHWM`；
性能对比时使用相同驱动、窗口尺寸和数据目录，不启用截图（截图会额外分配位图）。
本次 SDL offscreen 环境、1280×800 空工作空间测试结果：

| 项目 | 修改前 | 修改后 |
| --- | ---: | ---: |
| 启动峰值 RSS | 182100 KiB | 103808 KiB |
| 稳态 RSS | 100884 KiB | 96676 KiB |

GPU/驱动内存计入 RSS 的方式随环境变化，不应将这些数据直接视为所有桌面的保证值。
实机还应检查浅色/深色界面、中文终端、线框符号和窗口缩放。
字体覆盖范围未裁剪，单通道纹理需要应用已要求的桌面 OpenGL 3.3。
