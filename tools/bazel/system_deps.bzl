"""Repository helpers for system-provided Qt and libmpv dependencies."""

_QT_MIN_VERSION = [6, 4, 0]

_QT_PACKAGES = [
    ("qt6_core", "Qt6Core", "Qt6::Core"),
    ("qt6_gui", "Qt6Gui", "Qt6::Gui"),
    ("qt6_quick", "Qt6Quick", "Qt6::Quick"),
    ("qt6_qml", "Qt6Qml", "Qt6::Qml"),
    ("qt6_test", "Qt6Test", "Qt6::Test"),
]

_MPV_PACKAGE = ("mpv", "mpv", "mpv")

def _split_flags(text):
    return [part for part in text.replace("\n", " ").split(" ") if part]

def _major_minor_patch(version):
    parts = version.split(".")
    parsed = []
    for part in parts[:3]:
        parsed.append(int(part))
    for _ in range(3 - len(parsed)):
        parsed.append(0)
    return parsed

def _version_at_least(version, minimum):
    parsed = _major_minor_patch(version)
    for index in range(3):
        if parsed[index] > minimum[index]:
            return True
        if parsed[index] < minimum[index]:
            return False
    return True

def _pkg_config_tool(repository_ctx):
    for tool in ["pkg-config", "pkgconf"]:
        path = repository_ctx.which(tool)
        if path:
            return path
    fail("""Unable to locate pkg-config/pkgconf while detecting Niconeon UI dependencies.

Install pkg-config (Linux) or mingw-w64-x86_64-pkgconf (MSYS2), then rerun Bazel.
If packages are installed in a non-standard prefix, pass PKG_CONFIG_PATH via --repo_env.""")

def _qt_tool(repository_ctx, tool_name):
    pkg_config_tool = _pkg_config_tool(repository_ctx)
    for variable in ["host_bins", "libexecdir", "prefix"]:
        result = repository_ctx.execute(
            [pkg_config_tool, "--variable={}".format(variable), "Qt6Core"],
            quiet = True,
        )
        if result.return_code == 0 and result.stdout.strip():
            base = result.stdout.strip()
            candidates = [
                "{}/{}".format(base, tool_name),
                "{}/bin/{}".format(base, tool_name),
                "{}/libexec/{}".format(base, tool_name),
            ]
            for candidate in candidates:
                if repository_ctx.path(candidate).exists:
                    return candidate

    for candidate in ["{}-qt6".format(tool_name), tool_name]:
        path = repository_ctx.which(candidate)
        if path:
            return path

    fail("""Unable to locate Qt tool `{tool_name}` while detecting Niconeon UI dependencies.

Install Qt 6 development tools, then rerun Bazel.
Linux packages used by CI include qt6-base-dev-tools and qt6-declarative-dev-tools.
MSYS2 packages used by CI include mingw-w64-x86_64-qt6-tools.
If Qt tools are installed in a non-standard prefix, pass PATH via --repo_env.""".format(tool_name = tool_name))

def _execute_pkg_config(repository_ctx, tool, args, display_name):
    result = repository_ctx.execute([tool] + args, quiet = True)
    if result.return_code == 0:
        return result.stdout.strip()
    fail("""Unable to locate {display_name} with pkg-config.

Command: {command}
Exit code: {exit_code}
stderr:
{stderr}

Install Qt 6.4+ development packages and libmpv development files.
Linux packages used by CI include pkg-config, libmpv-dev, qt6-base-dev, qt6-base-dev-tools,
qt6-declarative-dev, and qt6-declarative-dev-tools.
MSYS2 packages used by CI include mingw-w64-x86_64-pkgconf, mingw-w64-x86_64-qt6-base,
mingw-w64-x86_64-qt6-declarative, mingw-w64-x86_64-qt6-tools, and mingw-w64-x86_64-mpv.
If the package exists outside the default search path, pass PKG_CONFIG_PATH via --repo_env.""".format(
        command = " ".join([str(tool)] + args),
        display_name = display_name,
        exit_code = result.return_code,
        stderr = result.stderr.strip()
    ))

def _detect_package(repository_ctx, tool, target_name, pkg_name, display_name):
    version = _execute_pkg_config(repository_ctx, tool, ["--modversion", pkg_name], display_name)
    cflags = _split_flags(_execute_pkg_config(repository_ctx, tool, ["--cflags", pkg_name], display_name))
    libs = _split_flags(_execute_pkg_config(repository_ctx, tool, ["--libs", pkg_name], display_name))
    libdir = _execute_pkg_config(repository_ctx, tool, ["--variable=libdir", pkg_name], display_name)

    includes = []
    defines = []
    copts = []
    for flag in cflags:
        if flag.startswith("-I") and len(flag) > 2:
            includes.append(flag[2:])
        elif flag.startswith("-D") and len(flag) > 2:
            defines.append(flag[2:])
        else:
            copts.append(flag)

    return struct(
        target_name = target_name,
        pkg_name = pkg_name,
        display_name = display_name,
        version = version,
        includes = includes,
        defines = defines,
        copts = copts,
        linkopts = libs,
        libdir = libdir
    )

def _unique(items):
    result = []
    for item in items:
        if item and item not in result:
            result.append(item)
    return result

def _render_cc_library(dep, include_aliases):
    return """cc_library(
    name = "{target_name}",
    hdrs = glob(["include/**/*"]),
    includes = {includes},
    defines = {defines},
    copts = {copts},
    linkopts = {linkopts},
    data = [":runtime_paths"],
)
""".format(
        target_name = dep.target_name,
        includes = repr([include_aliases[include] for include in _unique(dep.includes)]),
        defines = repr(_unique(dep.defines)),
        copts = repr(_unique(dep.copts)),
        linkopts = repr(_unique(dep.linkopts))
    )

def _quote_shell(value):
    return "'" + value.replace("'", "'\\''") + "'"

def _render_tool_wrapper(path, extra_args = []):
    command = [_quote_shell(str(path))] + [_quote_shell(arg) for arg in extra_args]
    return "#!/usr/bin/env bash\nexec {} \"$@\"\n".format(" ".join(command))

def _system_deps_repository_impl(repository_ctx):
    if repository_ctx.os.name.lower().find("linux") == -1 and repository_ctx.os.name.lower().find("windows") == -1:
        fail("Niconeon Bazel Qt/libmpv dependency detection supports Linux and MSYS2 Windows only; detected platform: {}".format(repository_ctx.os.name))

    if repository_ctx.os.environ.get("NICONEON_DISABLE_MPV_DETECT") == "1":
        fail("mpv detection disabled by NICONEON_DISABLE_MPV_DETECT=1; remove the --repo_env override and ensure the mpv pkg-config package is installed.")

    tool = _pkg_config_tool(repository_ctx)
    detected = []
    for target_name, pkg_name, display_name in _QT_PACKAGES:
        dep = _detect_package(repository_ctx, tool, target_name, pkg_name, display_name)
        if not _version_at_least(dep.version, _QT_MIN_VERSION):
            fail("{} version {} is too old; Niconeon requires Qt 6.4 or newer.".format(display_name, dep.version))
        detected.append(dep)

    target_name, pkg_name, display_name = _MPV_PACKAGE
    detected.append(_detect_package(repository_ctx, tool, target_name, pkg_name, display_name))

    runtime_dirs = _unique([dep.libdir for dep in detected])
    include_aliases = {}
    for index, include in enumerate(_unique([include for dep in detected for include in dep.includes])):
        alias = "include/{}".format(index)
        repository_ctx.symlink(include, alias)
        include_aliases[include] = alias

    moc_flags = []
    for dep in detected:
        moc_flags.extend(["-I{}".format(include) for include in dep.includes])
        moc_flags.extend(["-D{}".format(define) for define in dep.defines])

    repository_ctx.file("runtime_paths.txt", "\n".join(runtime_dirs) + "\n")
    repository_ctx.file("moc", _render_tool_wrapper(_qt_tool(repository_ctx, "moc"), _unique(moc_flags)), executable = True)
    repository_ctx.file("rcc", _render_tool_wrapper(_qt_tool(repository_ctx, "rcc")), executable = True)
    repository_ctx.file("BUILD.bazel", """load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

exports_files([
    "moc",
    "rcc",
])

filegroup(
    name = "runtime_paths",
    srcs = ["runtime_paths.txt"],
)

{libraries}
cc_library(
    name = "qt_mpv_deps",
    deps = [
        ":qt6_quick",
        ":qt6_qml",
        ":qt6_test",
        ":mpv",
    ],
    data = [":runtime_paths"],
)
""".format(libraries = "\n".join([_render_cc_library(dep, include_aliases) for dep in detected])))

system_deps_repository = repository_rule(
    implementation = _system_deps_repository_impl,
    environ = [
        "NICONEON_DISABLE_MPV_DETECT",
        "PATH",
        "PKG_CONFIG_PATH",
        "PKG_CONFIG_LIBDIR",
        "PKG_CONFIG_SYSROOT_DIR",
    ],
)

def _system_deps_extension_impl(module_ctx):
    system_deps_repository(name = "niconeon_system_deps")

system_deps = module_extension(implementation = _system_deps_extension_impl)
