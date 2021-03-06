# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Defines the rule “emacs_binary”, which compiles Emacs for use in Bazel."""

load("@bazel_skylib//lib:paths.bzl", "paths")
load(
    "@bazel_tools//tools/build_defs/cc:action_names.bzl",
    "CPP_LINK_EXECUTABLE_ACTION_NAME",
    "C_COMPILE_ACTION_NAME",
)
load(
    "//elisp:util.bzl",
    "cc_wrapper",
    "configure_cc_toolchain",
    "cpp_string",
    "runfile_location",
)

def _emacs_binary_impl(ctx):
    """Rule implementation of the “emacs_binary” rule."""
    cc_toolchain, feature_configuration = configure_cc_toolchain(ctx)

    # It’s not possible to refer to a directory as a label, so we refer to a
    # known file (README in the source root) instead.
    readme = ctx.file.readme
    source = "./{}/{}".format(readme.root.path, readme.dirname)
    install = _install(ctx, cc_toolchain, feature_configuration, source)
    driver = ctx.actions.declare_file("_" + ctx.label.name + ".cc")
    ctx.actions.expand_template(
        template = ctx.file._template,
        output = driver,
        substitutions = {
            "[[install]]": cpp_string(runfile_location(ctx, install)),
        },
        is_executable = True,
    )
    executable = cc_wrapper(ctx, cc_toolchain, feature_configuration, driver)
    return DefaultInfo(
        executable = executable,
        files = depset(direct = [executable]),
        runfiles = ctx.runfiles(files = [install]),
    )

emacs_binary = rule(
    attrs = {
        "srcs": attr.label_list(
            allow_files = True,
            allow_empty = False,
            mandatory = True,
            doc = "All Emacs source files.",
        ),
        "readme": attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = """The README file in the root of the Emacs repository.
This is necessary to determine the source root directory.""",
        ),
        "_build": attr.label(
            default = "//emacs:build",
            executable = True,
            cfg = "host",
        ),
        "_cc_toolchain": attr.label(
            default = "@bazel_tools//tools/cpp:current_cc_toolchain",
            providers = [cc_common.CcToolchainInfo],
        ),
        "_grep_includes": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "host",
            default = Label("@bazel_tools//tools/cpp:grep-includes"),
        ),
        "_exec": attr.label(
            default = "//elisp:exec",
            providers = [CcInfo],
        ),
        "_template": attr.label(
            default = "//emacs:driver.template",
            allow_single_file = [".template"],
        ),
    },
    doc = """Builds Emacs from a source repository.
The resulting executable can be used to run the compiled Emacs.""",
    executable = True,
    fragments = ["cpp"],
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    incompatible_use_toolchain_transition = True,
    implementation = _emacs_binary_impl,
)

def _install(ctx, cc_toolchain, feature_configuration, source):
    """Builds and install Emacs.

    Args:
      ctx (ctx): rule context
      cc_toolchain (Provider): the C toolchain to use to compile Emacs
      feature_configuration (FeatureConfiguration): the features to use to
          compile Emacs
      source (File): the directory containing the Emacs source tree

    Returns:
      a File representing the Emacs installation directory
    """
    fragment = ctx.fragments.cpp
    vars = cc_common.create_compile_variables(
        cc_toolchain = cc_toolchain,
        feature_configuration = feature_configuration,
    )
    cc = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = C_COMPILE_ACTION_NAME,
    )
    cflags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = C_COMPILE_ACTION_NAME,
        variables = vars,
    ) + fragment.copts + fragment.conlyopts
    ldflags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = CPP_LINK_EXECUTABLE_ACTION_NAME,
        variables = vars,
    ) + fragment.linkopts
    # The build process needs to find some common binaries like “make” or the
    # GNU coreutils.
    env = {"PATH": "/usr/bin:/bin"}
    for action in (C_COMPILE_ACTION_NAME, CPP_LINK_EXECUTABLE_ACTION_NAME):
        env.update(cc_common.get_environment_variables(
            feature_configuration = feature_configuration,
            action_name = action,
            variables = vars,
        ))
    install = ctx.actions.declare_directory(
        "_{}_install".format(ctx.label.name),
    )
    ctx.actions.run(
        outputs = [install],
        inputs = depset(
            direct = ctx.files.srcs,
            transitive = [cc_toolchain.all_files],
        ),
        executable = ctx.executable._build,
        arguments = [
            "--source=" + source,
            "--install=" + install.path,
            "--cc=" + cc,
            "--cflags=" + " ".join(cflags),
            "--ldflags=" + " ".join(ldflags),
        ],
        mnemonic = "EmacsInstall",
        progress_message = (
            "Installing Emacs into {}".format(install.short_path)
        ),
        env = env,
    )
    return install
