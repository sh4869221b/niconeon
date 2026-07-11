"""Release packaging staging helpers."""

def _binary_package_dir_impl(ctx):
    output_dir = ctx.actions.declare_directory(ctx.attr.dirname)
    binary = ctx.executable.binary

    ctx.actions.run_shell(
        inputs = [binary],
        outputs = [output_dir],
        arguments = [
            binary.path,
            output_dir.path,
            ctx.attr.output_name,
        ],
        command = """\
set -euo pipefail
rm -rf "$2"
mkdir -p "$2"
cp "$1" "$2/$3"
chmod 755 "$2/$3"
""",
    )

    return DefaultInfo(files = depset([output_dir]))

binary_package_dir = rule(
    implementation = _binary_package_dir_impl,
    attrs = {
        "binary": attr.label(
            allow_files = True,
            cfg = "target",
            executable = True,
            mandatory = True,
        ),
        "dirname": attr.string(mandatory = True),
        "output_name": attr.string(mandatory = True),
    },
)
