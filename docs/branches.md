# 分支说明

本仓库是 COS 318（Operating Systems，Fall 2013）课程项目 `Operativsystemer`。
目前有三个分支，它们都源自同一个初始提交 `da04424`（*Initial commit. Projects unaltered.*），
也就是课程下发、未经任何修改的项目骨架。

| 分支 | 提交数 | 目录结构 | 概况 |
| --- | --- | --- | --- |
| `main` | 1 | `cos318-project1/` ~ `cos318-project6/` | 原始骨架，无实现 |
| `Implementering1` | 5 | 同 `main` | 只改动 project1，挪威语提交信息 |
| `JH` | 12 | `0_pre/`、`1_project/` ~ `6_project/` 等 | 重命名目录并做了大量实现 |

```
da04424  Initial commit. Projects unaltered.
   ├── main                 (默认分支，停在此提交)
   ├── Implementering1      (+4 commits)
   └── JH                   (+11 commits，含一次 merge)
```

## main

- 只有 1 个提交，是仓库的默认分支，内容完全未改动。
- 六个目录 `cos318-project1` ~ `cos318-project6` 对应课程六个 project。
- 以 project1 为例：`createimage.c` 是标注 *Largely unimplemented* 的占位实现（99 行），
  `Makefile` 的 `image` 目标调用预编译好的课程给定工具 `./createimage.given --extended ./bootblock ./kernel`
  来生成软盘镜像 `image`。
- `bootblock.s` / `kernel.s` 是 16 位实模式汇编骨架，`bootblock`、`kernel`、`image` 等二进制已随仓库提供。

## Implementering1

- 从 `main` 拉出，新增 4 个提交，提交信息为挪威语：
  - `da0e649` *ingenting fungerer*（什么都不工作）
  - `ad1e8a7` *Bootlock*
  - `6cdc91a` *Fungerer. Implementasjon er til å gråte av.*（能跑了，但实现写得很糟）
  - `2f929fd` *Oppdatert og fjernet...*（更新并删掉一些东西；程序仍无法从启动处正常跑起来）
- 改动集中在 `cos318-project1/`，共 12 个文件、+506 / -37 行：
  - `createimage.c` 从 99 行占位实现扩写为 226 行，并新增 `createimage2.c`（244 行）作为另一版实现；
  - `bootblock.s` 大幅改写（+127 行），新增 `.vscode/settings.json`；
  - `bochsrc` 重命名为 `bochsrc2`，新增对比用的 `imagegoal`；
  - `Makefile` 的 `image` 目标由 `./createimage.given` 改为自己编译的 `./createimage`。
- 目录命名仍保持 `cos318-projectN`，其余 project 未动。

## JH

- 同样从 `main` 拉出，共 12 个提交，提交信息为英文，中途有一次冲突解决与合并
  （`8f790e0` *Trying to resolve merge*、`effc740` *Merged*）。
- 整体重命名了目录：`0_pre`、`1_project` ~ `6_project`，另外加入 `NOTATER.txt`、
  `commands.txt`、`serial.out`、`bochs_files/`、`tools/` 等辅助文件，
  并把大量 `.o`、`.swp`、`image.hex` 等中间产物也纳入了版本控制。
- 主要提交脉络：bootblock 栈修复（`a16ea5d`）→ 实现 createimage，尚无 `--extended`（`ad3b93f`）
  → 除 `--extended` 外都能工作、随机试出一个可用栈段（`0b0f98a`）→ bootloader 勉强能动（`90f260a`）
  → 补入 bochs 配置（`8431f64`）→ 修正 createimage 的内存错误、成功生成镜像（`99ed7b3`）。
- `1_project/Makefile` 也改为 `./createimage --extended ./bootblock ./kernel`，
  `createimage.c` 为 176 行的实现。
- 相对 `main` 差异最大：358 个文件变化、+2274 / -356 行，是三个分支中完成度最高的一支。

## 派生分支

为完成 lab，分别基于上述三个分支创建了 `edidada/main`、`edidada/Implementering1`、
`edidada/JH`。每个分支都新增了本说明文档和 `.github/workflows/build.yml`：
在 GitHub Actions 的 `ubuntu-latest` 上编译 project1（`gcc-multilib` + `ld -melf_i386`），
运行 `createimage` 生成 `image` 并校验引导扇区签名 `0x55aa`。
原分支内容不作任何改动。
