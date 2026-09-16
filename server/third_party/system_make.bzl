"""把系统 make 包装成 rules_foreign_cc 能接受的树形产物。

为什么需要它:rules_foreign_cc 的 make toolchain 期待一个「构建产物树」
(`target` 是 tree artifact,`path = $(execpath :target)/bin/make`),它既用它
跑 configure_make,也用 `target.files` 作为自举 pkg-config 的输入
(foreign_cc/built_tools/pkgconfig_build.bzl:91)。

macOS 上不能再用 rules_foreign_cc 自举的 GNU Make 4.4.1 —— 它跑 FFmpeg 的
Makefile 会段错误(已实测:自举 make dry-run 退出码 139,Apple 自带 make 退出码 0)。
但直接把 `path` 指向 /usr/bin/make 也不行:toolchain 解析会把绝对路径当成相对
execroot 的路径,拼出 `$EXT_BUILD_ROOT//usr/bin/make` 这种不存在的路径。

所以这里生成一个最小树产物,里面放一个转发到系统 make 的 shim,行为和内置
`built_make` toolchain 完全同构。
"""

def _system_make_shim_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run_shell(
        outputs = [out],
        command = "\n".join([
            "mkdir -p {}/bin".format(out.path),
            "printf '%s\\n' '#!/bin/sh' 'exec /usr/bin/make \"$@\"' > {}/bin/make".format(out.path),
            "chmod +x {}/bin/make".format(out.path),
        ]),
        mnemonic = "SystemMakeShim",
        # 进度文案保持 ASCII:Bazel 以 Latin-1 读 .bzl 源码,中文在输出里会变乱码。
        progress_message = "Generating system make shim",
    )
    return [DefaultInfo(files = depset([out]))]

system_make_shim = rule(
    implementation = _system_make_shim_impl,
    doc = "产出 bin/make(转发到 /usr/bin/make)的树产物,供 macOS make toolchain 使用。",
)
