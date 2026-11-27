def _okc_repo_impl(repository_ctx):
    sources = [
        "okc.c",
        "space-cc.c",
        "space-transform.c",
        "space_rewrite.c",
        "space-cc.h",
        "space-transform.h",
        "space_rewrite.h",
    ]
    for name in sources:
        repository_ctx.symlink(
            repository_ctx.path(Label("//:" + name)), name)
    clang = repository_ctx.which("clang")
    if clang == None:
        fail("okc_repo: clang not found in PATH")
    result = repository_ctx.execute([
        str(clang), "-std=c23", "-O2", "-o", "okc",
        "okc.c", "space-cc.c", "space-transform.c", "space_rewrite.c",
    ])
    if result.return_code != 0:
        fail("okc_repo: failed to build okc: " + result.stderr)
    repository_ctx.symlink("okc", "clang")
    repository_ctx.file("BUILD.bazel", 'exports_files(["okc", "clang"])\n')

okc_repo = repository_rule(implementation = _okc_repo_impl, configure = True)
