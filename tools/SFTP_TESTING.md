# SFTP 验证

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
