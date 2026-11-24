# SFTP 验证

## 同一 SSH 连接内暂停与恢复（Linux 自动回环测试）

不需要系统账号、sshd 或真实远端。测试仅监听 127.0.0.1 随机端口，使用临时 ECDSA 主机密钥、独立数据库和私有临时目录；退出后清理。服务端延迟每次 SFTP 读写，让客户端可以在**已传输部分数据**之后请求暂停，而不是仅在队列中暂停。

在项目根目录使用 Bash 和项目要求的 Zig 版本（x86_64 Linux）。下面直接编译实际控制器与网络代码，不读取或覆盖正式构建的对象文件：

```bash
set -e
obj=build/test_sftp_pause_obj
target=x86_64-linux-gnu.2.35
mkdir -p "$obj"
export ZIG_GLOBAL_CACHE_DIR="$PWD/$obj/zig-cache" ZIG_LOCAL_CACHE_DIR="$PWD/$obj/local-cache"
flags=(-target "$target" -std=c++20 -O1 -pthread -Iinclude -Iinclude/vendor/linux)
zig build-lib src/zig/crypto.zig -target "$target" -lc -O ReleaseSafe -femit-bin="$obj/libcredential_crypto.a"
for src in src/app/{workspace_controller,terminal,host_service,local_files,credential_service,private_key_file}.cpp \
           src/metrics/collector.cpp src/store/database.cpp src/net/{ssh_session,serial_session}.cpp; do
    zig c++ "${flags[@]}" -c "$src" -o "$obj/$(basename "$src" .cpp).o"
done
zig c++ "${flags[@]}" tools/test_sftp_pause.cpp "$obj"/{workspace_controller,terminal,host_service,local_files,credential_service,private_key_file,collector,database,ssh_session,serial_session}.o \
    lib/linux/lib{vterm,sqlite3,ssh2,mbedtls,mbedx509,mbedcrypto,everest,p256m,z}.a \
    "$obj/libcredential_crypto.a" -ldl -o build/test_sftp_pause
```

Python 3.9+，测试依赖 Paramiko（本地验证为 5.1.0）。若环境未安装，可创建仅测试使用的虚拟环境：

```bash
python3 -m venv build/test_sftp_pause_venv
build/test_sftp_pause_venv/bin/pip install paramiko==5.1.0
build/test_sftp_pause_venv/bin/python tools/test_sftp_pause.py build/test_sftp_pause
# 已通过 pip --target build/test-python 安装时也可直接运行：
# PYTHONPATH=build/test-python python3 tools/test_sftp_pause.py build/test_sftp_pause
```

测试退出码为 0 表示：

- 控制器队列可以跳过**尚未提交**的暂停项；已在传输的暂停项则保留任务槽、拒绝重复入队，且不会被“清除已完成”移除。
- 上传和下载都在中途确认暂停；字节进度保持不变，终端仍可回显输入，目标文件尚未发布。恢复后进度不倒退，最终文件与固定种子的二进制数据逐字节相同。服务端记录句柄打开次数，并校验没有额外 SSH 连接。
- 从双向暂停状态取消，不发布最终文件、并清理临时文件；传输中断线后不能恢复原任务，本地下载临时文件已清理。
- 网络层在活动任务暂停时也可取消仍在等待的暂停任务；活动任务在 2 秒预算下暂停 2.4 秒仍可成功恢复。未暂停的慢速任务超过 200 毫秒后按时进入失败清理，连接仍可用。

不测跨连接续传；本测试通过界面使用的回调直接驱动控制器，不依赖 GUI 或截图。

## 既有目录与传输回归（OpenSSH）

先运行 `./build.sh linux`，再链接控制器集成测试：

```bash
ZIG_GLOBAL_CACHE_DIR="$PWD/build/zig-cache" zig c++ \
  -target x86_64-linux-gnu.2.35 -std=c++20 -O2 -pthread -Iinclude \
  tools/test_sftp_workspace.cpp \
  build/obj/{workspace_controller,terminal,host_service,local_files,credential_service,private_key_file,collector,database,ssh_session,serial_session}.o \
  lib/linux/lib{vterm,sqlite3,ssh2,mbedtls,mbedx509,mbedcrypto,everest,p256m,z}.a \
  build/credentials/x86_64-linux-gnu.2.35/libcredential_crypto.a -ldl \
  -o build/test_sftp_workspace
```

用仅监听 `127.0.0.1` 的临时 OpenSSH 服务（`Subsystem sftp internal-sftp`），让服务端用户和测试进程可以访问同一个私有临时目录。使用临时 ECDSA P-256 主机密钥和客户端密钥；客户端私钥用 `ssh-keygen -t ecdsa -b 256 -m PEM` 生成。**测试会在指定目录中创建 `local`、`remote`、`data`，并自动信任这个临时服务器。请勿使用已有数据目录或生产服务。**

```bash
build/test_sftp_workspace PORT USER PRIVATE_KEY $(mktemp -d /tmp/jianssh-sftp.XXXXXX)
```

测试覆盖目录规范化、导航失败后保留原列表、文件元数据、排队去重和取消、上传、下载、同名防覆盖、目标侧刷新及远端新建目录。

手动界面检查：切换两个 SSH 标签页确认目录与筛选分别保留；点击表头切换排序；用 Ctrl/Shift 多选并传输；拖单个文件到另一栏的目录行；在窄窗口确认可滚动浏览两个栏和队列；右键重命名、复制路径或删除测试文件。
