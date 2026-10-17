"""Compile tests for Rust: snippets that must, or must not, compile against a crate.

A compile test checks what the type system accepts rather than what code does at run time: that
a lifetime-branded handle cannot leave its scope, that a `!Send` type cannot cross threads, that an
API meant to be usable still is. Each snippet is one .rs file, type-checked by rustc as a library
crate with `--emit=metadata` (as `cargo check` does: no codegen, no linking) against the crates in
`deps`. The check runs as a build action, so its result is cached like any other compilation; a
mismatch fails the action and Bazel prints the reason with rustc's output.

The snippet's annotations say which errors rustc must report, in the notation of rustc's own UI
tests:

    let local = lock.new_object();  //~ ERROR E0597
    //~^ ERROR E0597                (the error is on the previous line; one ^ per line up)
    //~| ERROR E0505                (another error on the same line as the previous annotation)
    //~? ERROR E0277                (anywhere: in a dependency's source, or with no location)

The text after `ERROR` is an error code, message text, or both (`E0597: does not live long`). A code
matches the error's code exactly and text matches a substring of its message. Prefer codes: they
are stable across rustc versions, where message text is not. Some errors have no code (borrowck's
"lifetime may not live long enough"); match those by message.

Every annotation must match an error rustc reports and every error rustc reports must match an
annotation, so a snippet cannot pass by failing for an unrelated reason such as a typo. A snippet
without annotations must compile. Warnings are not checked.

rustc borrow-checks only code that type-checks, and stops before either when names do not resolve,
so a snippet that expects a borrow error must be otherwise correct. Errors raised after type
checking (post-monomorphization const evaluation, linking) are out of reach: the snippet is never
code-generated.

A failing compile test fails to build (`bazel test` reports FAILED TO BUILD, with the checker's
report). A passing one leaves rustc's output in bazel-bin/<package>/<name>/<snippet>@check.stderr.
//build/rust/compile-tests holds the checker's own tests, which show each annotation form.
"""

load("@bazel_skylib//rules:build_test.bzl", "build_test")
load("@rules_rust//rust:defs.bzl", "rust_common")

# Not part of rules_rust's public API; rust_library reads --@rules_rust//:extra_rustc_flag(s)
# through it, and the snippets must be compiled with the same flags as their dependencies (some,
# like -Zsanitizer, have to match across crates).
load("@rules_rust//rust/private:rustc.bzl", "ExtraRustcFlagsInfo")

def _crate_dir(crate_info):
    return crate_info.output.dirname

def _dirname(file):
    return file.dirname

def _rust_compile_check_impl(ctx):
    toolchain = ctx.toolchains["@rules_rust//rust:toolchain_type"]
    src = ctx.file.src
    stamp = ctx.actions.declare_file(ctx.label.name + ".stderr")

    crate_infos = [dep[rust_common.crate_info] for dep in ctx.attr.deps + ctx.attr.proc_macro_deps]
    dep_infos = [dep[rust_common.dep_info] for dep in ctx.attr.deps + ctx.attr.proc_macro_deps]

    args = ctx.actions.args()
    if ctx.attr.expect_mismatch:
        args.add("--expect-mismatch")
    args.add(src)
    args.add(stamp)
    args.add("--")
    args.add(toolchain.rustc)
    args.add(src)
    args.add("--crate-name=compile_test_" + src.basename.removesuffix(".rs").replace("-", "_"))
    args.add("--crate-type=lib")
    args.add("--emit=metadata")
    args.add("--edition=" + (ctx.attr.edition or toolchain.default_edition))
    args.add("--target=" + toolchain.target_flag_value)
    if getattr(toolchain, "_toolchain_generated_sysroot", False):
        args.add_all([toolchain.sysroot_anchor], map_each = _dirname, format_each = "--sysroot=%s")
    args.add("--color=never")
    args.add("--error-format=human")

    args.add_all(toolchain.extra_rustc_flags_for_crate_types.get("lib", []))
    args.add_all(toolchain.extra_rustc_flags)
    args.add_all(ctx.attr._extra_rustc_flags[ExtraRustcFlagsInfo].extra_rustc_flags)
    args.add_all(ctx.attr._extra_rustc_flag[ExtraRustcFlagsInfo].extra_rustc_flags)

    # The snippet's warnings are noise in the report, even under --config=lint (-Dwarnings);
    # lints the snippet itself sets to deny still apply.
    args.add("-Awarnings")
    args.add_all(ctx.attr.rustc_flags)

    # Resolve each dependency from its rlib, as rules_rust does without pipelining. The direct
    # dependencies are `--extern`s the snippet can name; the rest are found through
    # -Ldependency, which keeps them out of the snippet's namespace.
    args.add_all(["--extern={}={}".format(c.name, c.output.path) for c in crate_infos])
    transitive_crates = depset(transitive = [d.transitive_crates for d in dep_infos])
    args.add_all(transitive_crates, map_each = _crate_dir, uniquify = True, format_each = "-Ldependency=%s")

    ctx.actions.run(
        executable = ctx.executable._checker,
        arguments = [args],
        inputs = depset(
            [src] + [c.output for c in crate_infos],
            transitive = [d.transitive_crate_outputs for d in dep_infos] + [toolchain.all_files],
        ),
        outputs = [stamp],
        env = toolchain.env,
        mnemonic = "RustCompileTest",
        progress_message = "Checking Rust compile test %{label}",
        toolchain = "@rules_rust//rust:toolchain_type",
    )
    return [DefaultInfo(files = depset([stamp]))]

_rust_compile_check = rule(
    implementation = _rust_compile_check_impl,
    attrs = {
        "deps": attr.label_list(providers = [rust_common.crate_info]),
        "edition": attr.string(),
        "expect_mismatch": attr.bool(),
        "proc_macro_deps": attr.label_list(cfg = "exec", providers = [rust_common.crate_info]),
        "rustc_flags": attr.string_list(),
        "src": attr.label(allow_single_file = [".rs"], mandatory = True),
        "_extra_rustc_flag": attr.label(default = "@rules_rust//rust/settings:extra_rustc_flag"),
        "_extra_rustc_flags": attr.label(default = "@rules_rust//rust/settings:extra_rustc_flags"),
        "_checker": attr.label(
            default = "//build/rust:compile_test_checker",
            executable = True,
            cfg = "exec",
        ),
    },
    toolchains = ["@rules_rust//rust:toolchain_type"],
)

def wd_rust_compile_test(
        name,
        srcs,
        deps = [],
        proc_macro_deps = [],
        edition = None,
        rustc_flags = [],
        expect_mismatch = False,
        tags = [],
        visibility = None):
    """Defines a compile test per snippet in `srcs`, and a test_suite `name` of all of them.

    Args:
        name: name of the test_suite; each snippet's test is <name>/<snippet's basename without .rs>.
        srcs: snippets, one .rs file each; see this file's docstring for their annotations.
        deps: the crates the snippets may `use`.
        proc_macro_deps: proc-macro crates the snippets may `use`.
        edition: Rust edition of the snippets; defaults to the toolchain's.
        rustc_flags: extra rustc flags for every snippet (e.g. `--cfg=...`).
        expect_mismatch: for the checker's own tests only: every snippet's annotations are wrong,
            and the test passes when the checker reports that.
        tags: tags for the tests.
        visibility: visibility of the test_suite.
    """
    tests = []
    for src in srcs:
        test_name = "{}/{}".format(name, src.rpartition("/")[2].removesuffix(".rs"))
        _rust_compile_check(
            name = test_name + "@check",
            src = src,
            deps = deps,
            proc_macro_deps = proc_macro_deps,
            edition = edition,
            rustc_flags = rustc_flags,
            expect_mismatch = expect_mismatch,
            tags = ["manual"],
            testonly = True,
            target_compatible_with = select({
                "@//build/config:no_build": ["@platforms//:incompatible"],
                "//conditions:default": [],
            }),
        )
        build_test(
            name = test_name,
            targets = [":" + test_name + "@check"],
            tags = tags,
        )
        tests.append(":" + test_name)
    native.test_suite(
        name = name,
        tests = tests,
        tags = tags,
        visibility = visibility,
    )
