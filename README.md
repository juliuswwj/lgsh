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

修改源码中的 `LGSH_BASE` 宏可更改基础目录路径（默认：`/home/lgsh`）。

### 目录结构

```
/home/lgsh/                    # 基础目录 (LGSH_BASE)
├── .config/
│   └── hosts                  # 自定义 hosts 文件（绑定挂载到 /etc/hosts）
└── <用户名>_<目录名>/         # 用户工作空间（按会话创建）
```

## 使用方法

从 `$HOME` 的子目录中运行：

```sh
# 默认：在隔离环境中启动 tmux
lgsh

# 运行指定程序
lgsh /bin/bash
lgsh /usr/bin/vim file.txt
```

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

4. **环境设置**
   - 绑定挂载自定义 hosts 文件到 `/etc/hosts`
   - 降权为目标 UID/GID
   - 设置 `HOME=/home/lgsh`
   - 切换到映射后的目录

5. **执行程序**
   - 运行指定程序或默认的 tmux

## 安全措施

程序实现了多层安全检查：

- **SUID root 检查**：必须以有效 UID 0 运行
- **root 排除**：不能直接以 root 用户运行
- **组限制**：只有可执行文件所属组的用户才能使用
- **工作目录验证**：必须在 `$HOME` 的子目录下运行

## 许可证

SPDX-License-Identifier: LGPL-2.1+