load("@clang_resource//:defs.bzl", "INCLUDES")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

def _okc_config_impl(ctx):
    base = ctx.attr.base[CcToolchainConfigInfo]
    tool_paths = []
    compiler_path = ctx.executable._compiler.short_path
    if compiler_path.startswith("../"):
        compiler_path = compiler_path.split("/", 2)[-1]
    for entry in base.tool_paths:
        path = compiler_path if entry.name == "gcc" else entry.path
        tool_paths.append(struct(name = entry.name, path = path, type_name = "tool_path"))
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = [],
        action_configs = [],
        artifact_name_patterns = [],
        cxx_builtin_include_directories = INCLUDES,
        toolchain_identifier = "okc",
        host_system_name = "local",
        target_system_name = base.target_system_name,
        target_cpu = base.target_cpu,
        target_libc = base.target_libc,
        compiler = base.compiler,
        abi_version = base.abi_version,
        abi_libc_version = base.abi_libc_version,
        tool_paths = tool_paths,
        builtin_sysroot = base.builtin_sysroot,
    )

okc_toolchain_config = rule(
    implementation = _okc_config_impl,
    attrs = {
        "base": attr.label(default = "@local_config_cc//:local"),
        "_compiler": attr.label(
            allow_files = True,
            executable = True,
            cfg = "exec",
            default = "//tools:clang",
        ),
    },
    fragments = ["cpp"],
    provides = [CcToolchainConfigInfo],
)
