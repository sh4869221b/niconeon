"""Small first-party Qt helpers for Bazel-owned Niconeon UI builds."""

def _label_safe_name(label):
    return label.replace("/", "_").replace(":", "_").replace(".", "_").replace("-", "_")

def _unique(values):
    result = []
    seen = {}
    for value in values:
        if value not in seen:
            seen[value] = True
            result.append(value)
    return result

def qt_moc_srcs(name, hdrs, extra_includes = {}, project_headers = []):
    """Generate C++ moc sources for QObject headers."""
    outs = []
    for hdr in hdrs:
        out = "{}_{}.moc.cpp".format(name, _label_safe_name(hdr))
        includes = extra_includes.get(hdr, [])
        include_args = " ".join(["-b $(rootpath {})".format(include) for include in includes])
        native.genrule(
            name = "{}_moc_{}".format(name, _label_safe_name(hdr)),
            srcs = _unique([hdr] + includes + project_headers),
            outs = [out],
            tools = ["@niconeon_system_deps//:moc"],
            cmd = "$(location @niconeon_system_deps//:moc) -I app-ui/src {} -f $(rootpath {}) -o $@ $(location {})".format(include_args, hdr, hdr),
        )
        outs.append(out)
    return outs

def qt_inline_moc_headers(name, srcs, project_headers = []):
    """Generate .moc files included by Qt test sources."""
    outs = []
    for src in srcs:
        out = src[:-4] + ".moc.h" if src.endswith(".cpp") else "{}.moc.h".format(src)
        native.genrule(
            name = "{}_inline_moc_{}".format(name, _label_safe_name(src)),
            srcs = [src] + project_headers,
            outs = [out],
            tools = ["@niconeon_system_deps//:moc"],
            cmd = "$(location @niconeon_system_deps//:moc) -I app-ui/src -o $@ $(location {})".format(src),
        )
        outs.append(out)
    return outs

def qt_resource_src(name, qrc, srcs = []):
    """Generate a C++ source from a Qt .qrc file."""
    out = "{}.qrc.cpp".format(name)
    native.genrule(
        name = "{}_rcc".format(name),
        srcs = [qrc] + srcs,
        outs = [out],
        tools = ["@niconeon_system_deps//:rcc"],
        cmd = "$(location @niconeon_system_deps//:rcc) -name {} -o $@ $(location {})".format(name, qrc),
    )
    return out
