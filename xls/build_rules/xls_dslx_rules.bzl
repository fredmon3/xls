# Copyright 2021 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""This module contains DSLX-related build rules for XLS."""

load("@bazel_skylib//lib:dicts.bzl", "dicts")
load(
    "//xls/build_rules:xls_common_rules.bzl",
    "append_cmd_line_args_to",
    "append_default_to_args",
    "args_to_string",
    "get_runfiles_for_xls",
    "get_transitive_built_files_for_xls",
    "is_args_valid",
)
load(
    "//xls/build_rules:xls_providers.bzl",
    "DslxInfo",
)
load(
    "//xls/build_rules:xls_toolchains.bzl",
    "xls_toolchain_attrs",
)

_DEFAULT_DSLX_TEST_ARGS = {
    "compare": "jit",
}

def get_transitive_dslx_srcs_files_depset(srcs, deps):
    """Returns a depset representing the transitive DSLX source files.

    The macro is used to collect the transitive DSLX source files of a target.

    Args:
      srcs: a list of DSLX source files (.x)
      deps: a list of targets

    Returns:
      A depset collection where the files from 'srcs' are placed in the 'direct'
      field of the depset and the DSLX source files for each dependency in
      'deps' are placed in the 'transitive' field of the depset.
    """
    return depset(
        srcs,
        transitive = [dep[DslxInfo].dslx_source_files for dep in deps],
    )

def get_transitive_dslx_placeholder_files_depset(srcs, deps):
    """Returns a depset representing the transitive DSLX placeholder files.

    The macro is used to collect the transitive DSLX placeholder files of a target.

    Args:
      srcs: a list of DSLX placeholder files (.placeholder)
      deps: a list of targets dependencies

    Returns:
      A depset collection where the files from 'srcs' are placed in the 'direct'
      field of the depset and the DSLX placeholder files for each dependency in 'deps'
      are placed in the 'transitive' field of the depset.
    """
    return depset(
        srcs,
        transitive = [dep[DslxInfo].dslx_placeholder_files for dep in deps],
    )

def _get_dslx_test_cmdline(ctx, src, append_cmd_line_args = True):
    """Returns the command that executes in the xls_dslx_test rule.

    Args:
      ctx: The current rule's context object.
      src: The file to test.
      append_cmd_line_args: Flag controlling appending the command-line
        arguments invoking the command generated by this function. When set to
        True, the command-line arguments invoking the command are appended.
        Otherwise, the command-line arguments are not appended.

    Returns:
      The command that executes in the xls_dslx_test rule.
    """
    dslx_interpreter_tool = ctx.executable._xls_dslx_interpreter_tool
    _dslx_test_args = append_default_to_args(
        ctx.attr.dslx_test_args,
        _DEFAULT_DSLX_TEST_ARGS,
    )

    DSLX_TEST_FLAGS = (
        "compare",
        "dslx_path",
        "warnings_as_errors",
        "disable_warnings",
        "max_ticks",
        "format_preference",
    )

    dslx_test_args = dict(_dslx_test_args)
    dslx_test_args["dslx_path"] = (
        dslx_test_args.get("dslx_path", "") + ":${PWD}:" +
        ctx.genfiles_dir.path + ":" + ctx.bin_dir.path
    )
    is_args_valid(dslx_test_args, DSLX_TEST_FLAGS)
    my_args = args_to_string(dslx_test_args)

    cmd = "{} {} {}".format(
        dslx_interpreter_tool.short_path,
        src.short_path,
        my_args,
    )

    # Append command-line arguments.
    if append_cmd_line_args:
        cmd = append_cmd_line_args_to(cmd)

    return cmd

def get_src_files_from_dslx_library_as_input(ctx):
    """Returns the DSLX source files of rules using 'xls_dslx_library_as_input_attrs'.

    Args:
      ctx: The current rule's context object.

    Returns:
      Returns the DSLX source files of rules using 'xls_dslx_library_as_input_attrs'.
    """
    dslx_src_files = []
    count = 0

    if ctx.attr.library:
        dslx_info = ctx.attr.library[DslxInfo]
        dslx_src_files = dslx_info.target_dslx_source_files
        count += 1
    if ctx.attr.srcs or ctx.attr.deps:
        if not ctx.attr.srcs:
            fail("'srcs' must be defined when 'deps' is defined.")
        dslx_src_files = ctx.files.srcs
        count += 1

    if count != 1:
        fail("One of: 'library' or ['srcs', 'deps'] must be assigned.")

    return dslx_src_files

def get_DslxInfo_from_dslx_library_as_input(ctx):
    """Returns the DslxInfo provider of rules using 'xls_dslx_library_as_input_attrs'.

    Args:
      ctx: The current rule's context object.

    Returns:
      DslxInfo provider
    """
    dslx_info = None
    count = 0

    if ctx.attr.library:
        dslx_info = ctx.attr.library[DslxInfo]
        count += 1
    if ctx.attr.srcs or ctx.attr.deps:
        if not ctx.attr.srcs:
            fail("'srcs' must be defined when 'deps' is defined.")
        dslx_info = DslxInfo(
            target_dslx_source_files = ctx.files.srcs,
            dslx_source_files = get_transitive_dslx_srcs_files_depset(
                ctx.files.srcs,
                ctx.attr.deps,
            ),
            dslx_placeholder_files = get_transitive_dslx_placeholder_files_depset(
                [],
                ctx.attr.deps,
            ),
        )
        count += 1

    if count != 1:
        fail("One of: 'library' or ['srcs', 'deps'] must be assigned.")

    return dslx_info

# Attributes for the xls_dslx_library rule.
_xls_dslx_library_attrs = {
    "srcs": attr.label_list(
        doc = "Source files for the rule. Files must have a '.x' extension.",
        allow_files = [".x"],
    ),
    "deps": attr.label_list(
        doc = "Dependency targets for the rule.",
        providers = [DslxInfo],
    ),
    "warnings_as_errors": attr.bool(
        doc = "Whether warnings are errors within this library definition.",
        mandatory = False,
    ),
}

xls_dslx_library_as_input_attrs = {
    "library": attr.label(
        doc = "A DSLX library target where the direct (non-transitive) " +
              "files of the target are tested. This attribute is mutually " +
              "exclusive with the 'srcs' and 'deps' attribute.",
        providers = [DslxInfo],
    ),
    "srcs": attr.label_list(
        doc = "Source files for the rule. The files must have a '.x' " +
              "extension. This attribute is mutually exclusive with the " +
              "'library' attribute.",
        allow_files = [".x"],
    ),
    "deps": attr.label_list(
        doc = "Dependency targets for the files in the 'srcs' attribute. " +
              "This attribute is mutually exclusive with the 'library' " +
              "attribute.",
        providers = [DslxInfo],
    ),
}

# Common attributes for DSLX testing.
xls_dslx_test_common_attrs = {
    "dslx_test_args": attr.string_dict(
        doc = "Arguments of the DSLX interpreter executable. For details " +
              "on the arguments, refer to the interpreter_main " +
              "application at " +
              "//xls/dslx/interpreter_main.cc.",
    ),
}

#TODO(https://github.com/google/xls/issues/392) 04-14-21
def _xls_dslx_library_impl(ctx):
    """The implementation of the 'xls_dslx_library' rule.

    Parses and type checks DSLX source files. When a DSLX file is successfully
    parsed and type checked, a DSLX placeholder file is generated. The placeholder file is
    used to create a dependency between the current target and the target's
    descendants.

    Args:
      ctx: The current rule's context object.

    Returns:
      DslxInfo provider
      DefaultInfo provider
    """

    # Get runfiles for task.
    dslx_interpreter_tool_runfiles = (
        ctx.attr._xls_dslx_interpreter_tool[DefaultInfo].default_runfiles
    )
    runfiles = get_runfiles_for_xls(
        ctx = ctx,
        additional_runfiles_list = [dslx_interpreter_tool_runfiles],
        additional_files_list = [],
    )

    my_srcs_list = ctx.files.srcs
    dslx_interpreter_tool = ctx.executable._xls_dslx_interpreter_tool

    # Parse and type check the DSLX source files.
    dslx_srcs_str = " ".join([s.path for s in my_srcs_list])
    placeholder_file = ctx.actions.declare_file(ctx.attr.name + ".placeholder")

    # With runs outside a monorepo, the execution root for the workspace of
    # the binary can be different with the execroot, requiring to change
    # the dslx stdlib search path accordingly.
    # e.g., Label("@repo//pkg/xls:binary").workspace_root == "external/repo"
    wsroot = ctx.attr._xls_dslx_interpreter_tool.label.workspace_root
    wsroot_dslx_path = ":{}".format(wsroot) if wsroot != "" else ""
    dslx_srcs_wsroot = ":".join([s.owner.workspace_root for s in my_srcs_list])
    dslx_srcs_wsroot_path = ":{}".format(dslx_srcs_wsroot) if dslx_srcs_wsroot != "" else ""

    ctx.actions.run_shell(
        outputs = [placeholder_file],
        # The DSLX interpreter executable is a tool needed by the action.
        tools = [dslx_interpreter_tool],
        # The files required for parsing and type checking also requires the
        # DSLX interpreter executable.
        inputs = runfiles.files,
        # Generate a placeholder file for the DSLX source file when the source file is
        # successfully parsed and type checked.
        # TODO (vmirian) 01-05-21 Enable the interpreter to take multiple files.
        # TODO (vmirian) 01-05-21 Ideally, create a standalone tool that parses
        # a DSLX file. (Instead of repurposing the interpreter.)
        command = "\n".join([
            "FILES=\"{}\"".format(dslx_srcs_str),
            "for file in $FILES; do",
            "{} $file --compare=none --execute=false --dslx_path={}{}".format(
                dslx_interpreter_tool.path,
                ":${PWD}:" + ctx.genfiles_dir.path + ":" + ctx.bin_dir.path +
                dslx_srcs_wsroot_path + wsroot_dslx_path,
                " --warnings_as_errors=false" if not ctx.attr.warnings_as_errors else "",
            ),
            "if [ $? -ne 0 ]; then",
            "echo \"Error parsing and type checking DSLX source file: $file\"",
            "exit -1",
            "fi",
            "done",
            "touch {}".format(placeholder_file.path),
            "exit 0",
        ]),
        mnemonic = "ParseAndTypeCheckDSLXSourceFile",
        progress_message = "Parsing and type checking DSLX source files of " +
                           "target %s" % (ctx.attr.name),
        toolchain = None,
    )

    placeholder_files_depset = get_transitive_dslx_placeholder_files_depset(
        [placeholder_file],
        ctx.attr.deps,
    )
    return [
        DslxInfo(
            target_dslx_source_files = my_srcs_list,
            dslx_source_files = get_transitive_dslx_srcs_files_depset(
                my_srcs_list,
                ctx.attr.deps,
            ),
            dslx_placeholder_files = placeholder_files_depset,
        ),
        DefaultInfo(
            files = placeholder_files_depset,
            runfiles = runfiles,
        ),
    ]

xls_dslx_library = rule(
    doc = """A build rule that parses and type checks DSLX source files.

Examples:

1. A collection of DSLX source files.

    ```
    xls_dslx_library(
        name = "files_123_dslx",
        srcs = [
            "file_1.x",
            "file_2.x",
            "file_3.x",
        ],
    )
    ```

1. Dependency on other xls_dslx_library targets.

    ```
    xls_dslx_library(
        name = "a_dslx",
        srcs = ["a.x"],
    )

    # Depends on target a_dslx.
    xls_dslx_library(
        name = "b_dslx",
        srcs = ["b.x"],
        deps = [":a_dslx"],
    )

    # Depends on target a_dslx.
    xls_dslx_library(
        name = "c_dslx",
        srcs = ["c.x"],
        deps = [":a_dslx"],
    )
    ```
    """,
    implementation = _xls_dslx_library_impl,
    attrs = dicts.add(
        _xls_dslx_library_attrs,
        xls_toolchain_attrs,
    ),
)

def get_dslx_test_cmd(ctx, src_files_to_test):
    """Returns the runfiles and commands to execute the sources files.

    Args:
      ctx: The current rule's context object.
      src_files_to_test: A list of source files to test.

    Returns:
      A tuple with the following elements in the order presented:
        1. The runfiles to execute the commands.
        1. A list of commands.
    """

    # Get runfiles.
    dslx_interpreter_tool_runfiles = (
        ctx.attr._xls_dslx_interpreter_tool[DefaultInfo].default_runfiles
    )
    runfiles = get_runfiles_for_xls(
        ctx,
        [dslx_interpreter_tool_runfiles],
        src_files_to_test,
    )

    cmds = []
    for src in src_files_to_test:
        cmds.append(_get_dslx_test_cmdline(ctx, src))
    return runfiles, cmds

def _xls_dslx_test_impl(ctx):
    """The implementation of the 'xls_dslx_test' rule.

    Executes the tests and quick checks of a DSLX source file.

    Args:
      ctx: The current rule's context object.

    Returns:
      DefaultInfo provider
    """
    src_files_to_test = get_src_files_from_dslx_library_as_input(ctx)

    runfiles, cmds = get_dslx_test_cmd(ctx, src_files_to_test)

    executable_file = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = executable_file,
        content = "\n".join([
            "#!/usr/bin/env bash",
            "set -e",
            "\n".join(cmds),
            "exit 0",
        ]),
        is_executable = True,
    )
    return [
        DefaultInfo(
            runfiles = runfiles,
            files = depset(
                direct = [executable_file],
                transitive = get_transitive_built_files_for_xls(ctx),
            ),
            executable = executable_file,
        ),
    ]

xls_dslx_test = rule(
    doc = """A dslx test executes the tests and quick checks of a DSLX source file.

Examples:

1. xls_dslx_test on DSLX source files.

    ```
    # Assume a xls_dslx_library target bc_dslx is present.
    xls_dslx_test(
        name = "e_dslx_test",
        srcs = [
            "d.x",
            "e.x",
        ],
        deps = [":bc_dslx"],
    )
    ```

1. xls_dslx_test on a xls_dslx_library.

    ```
    xls_dslx_library(
        name = "b_dslx",
        srcs = ["b.x"],
        deps = [":a_dslx"],
    )

    xls_dslx_test(
        name = "b_dslx_test",
        library = "b_dslx",
    )
    ```
    """,
    implementation = _xls_dslx_test_impl,
    attrs = dicts.add(
        xls_dslx_library_as_input_attrs,
        xls_dslx_test_common_attrs,
        xls_toolchain_attrs,
    ),
    test = True,
)
