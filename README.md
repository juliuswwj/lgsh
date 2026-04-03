# lgsh - 基于 ID 映射挂载的隔离 Shell 环境

一个 SUID 程序，利用 Linux idmapped mounts 创建隔离的 Shell 环境，让用户以不同的 UID/GID 身份操作文件。

## 简介

`lgsh` 允许非 root 用户通过 Linux 的 idmapped mount 功能，让文件看起来像是被不同的 UID/GID 所拥有。适用于：

- 需要特定文件所有权的开发环境
- 在隔离的用户环境中运行程序
- 测试不同的用户权限

## 系统要求

- Linux 内核 5.12+（支持 idmapped mounts）
- SUID root 二进制文件
- 可执行文件必须属于某个组，用户必须是该组成员

## 编译安装

```sh
gcc -o lgsh lgsh.c
sudo chown root:lgsh lgsh
sudo chmod 4750 lgsh
```

将 `lgsh` 替换为实际允许访问的组名。

## 配置

修改源码中的宏可更改配置：

- `LGSH_BASE` - 基础目录路径（默认：`/home/lgsh`）
- `DEFAULT_PATH` - 默认 PATH（默认：`/usr/local/cuda/bin:/usr/local/bin:/usr/bin:/bin:/snap/bin`）

### 目录结构

```
/home/lgsh/                    # 基础目录 (LGSH_BASE)
├── .config/
│   ├── hosts                  # 自定义 hosts（可选，挂载到 /etc/hosts）
│   └── resolv.conf            # 自定义 DNS（可选，挂载到 /etc/resolv.conf）
├── .local/bin/                # 用户自定义脚本（自动加入 PATH）
├── .nvm/versions/node/        # Node.js 版本（自动检测最新版）
├── miniconda3/                # Conda 环境
├── tools/
│   ├── ndk/                   # Android NDK（自动检测最新版）
│   ├── build-tools/           # Android build-tools（自动检测最新版）
│   ├── platform-tools/        # Android platform-tools
│   ├── cmdline-tools/latest/  # Android cmdline-tools
│   └── cmake/                 # CMake（自动检测最新版）
└── <用户名>_<目录名>/         # 用户工作空间（按会话创建）
```

## 环境变量

程序自动设置以下环境变量：

| 变量 | 说明 |
|------|------|
| `HOME` | `/home/lgsh` |
| `USER` | `lg` |
| `TZ` | `America/Los_Angeles`（自动处理夏令时） |
| `NVM_DIR` | `/home/lgsh/.nvm` |
| `NVM_BIN` | 最新 Node.js 的 bin 目录 |
| `NVM_INC` | 最新 Node.js 的 include 目录 |
| `VIRTUAL_ENV` | `/home/lgsh/miniconda3` |
| `ANDROID_NDK_HOME` | 最新 NDK 目录 |
| `NDK_ROOT` | 最新 NDK 目录 |
| `NDK_PROJECT_PATH` | 最新 NDK 目录 |
| `PWD` | 当前工作目录（映射后的路径） |
| `PATH` | 包含所有工具目录 |

### PATH 顺序

```
/home/lgsh/miniconda3/bin
/home/lgsh/.nvm/versions/node/<最新版>/bin
/home/lgsh/tools/ndk/<最新版>
/home/lgsh/tools/ndk/<最新版>/toolchains/llvm-prebuilt/linux-x86_64/bin
/home/lgsh/tools/build-tools/<最新版>
/home/lgsh/tools/platform-tools
/home/lgsh/tools/cmdline-tools/latest/bin
/home/lgsh/tools/cmake/<最新版>/bin
/home/lgsh/.local/bin
/usr/local/cuda/bin
/usr/local/bin
/usr/bin
/bin
/snap/bin
```

## 使用方法

从 `$HOME` 的子目录中运行：

```sh
# 默认：启动 tmux（自动 attach 或 create session）
lgsh

# 运行指定程序（支持 PATH 搜索，无需全路径）
lgsh bash
lgsh vim file.txt
lgsh python
```

### tmux session 管理

默认模式下，程序会自动管理 tmux session：

- session 名称：`<用户名>_<目录名>`（从工作目录提取）
- 如果 session 已存在 → `tmux attach`
- 如果 session 不存在 → `tmux new -s <名称>`

## 工作原理

1. **安全检查**
   - 验证程序以 SUID root 运行
   - 确认调用者不是 root
   - 检查用户是否属于可执行文件的组
   - 验证工作目录在 `$HOME` 下

2. **挂载命名空间**
   - 创建新的挂载命名空间
   - 将根挂载设置为私有传播

3. **ID 映射挂载**
   - 克隆当前工作目录
   - 应用 UID/GID 映射：调用者 ID → 目标 ID（来自 `/home/lgsh` 的所有权）
   - 挂载到 `/home/lgsh/<用户名>_<目录名>`

4. **配置挂载**
   - 可选绑定挂载 `/home/lgsh/.config/hosts` → `/etc/hosts`
   - 可选绑定挂载 `/home/lgsh/.config/resolv.conf` → `/etc/resolv.conf`

5. **目录切换与降权**
   - 切换到映射后的目录（root 权限下）
   - 设置 `$PWD` 环境变量
   - 降权为目标 UID/GID

6. **环境设置**
   - 设置 HOME、USER、TZ 等环境变量
   - 动态检测工具版本并设置相关环境变量
   - 构建 PATH

7. **执行程序**
   - 运行指定程序或默认的 tmux

## 安全措施

程序实现了多层安全检查：

- **SUID root 检查**：必须以有效 UID 0 运行
- **root 排除**：不能直接以 root 用户运行
- **组限制**：只有可执行文件所属组的用户才能使用
- **工作目录验证**：必须在 `$HOME` 的子目录下运行

## 许可证

SPDX-License-Identifier: LGPL-2.1+