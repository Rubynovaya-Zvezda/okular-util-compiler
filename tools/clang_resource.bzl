def _impl(repository_ctx):
    clang = repository_ctx.which("clang")
    if clang == None:
        fail("clang_resource: clang not found")
    result = repository_ctx.execute([
        "/bin/sh", "-c",
        str(clang) + " -E -x c -v - < /dev/null 2>&1",
    ])
    includes = []
    active = False
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("#include <...> search starts here:"):
            active = True
            continue
        if line.startswith("End of search list."):
            break
        if active and line:
            includes.append(line)
    resource = repository_ctx.execute([str(clang), "-print-resource-dir"])
    if resource.return_code == 0:
        includes.append(resource.stdout.strip() + "/include")
    repository_ctx.file("BUILD.bazel", "exports_files([\"defs.bzl\"])\n")
    body = "INCLUDES = [\n"
    for path in includes:
        body += "    \"" + path + "\",\n"
    body += "]\n"
    repository_ctx.file("defs.bzl", body)

clang_resource = repository_rule(implementation = _impl)
