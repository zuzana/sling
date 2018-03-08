package(default_visibility = ['//visibility:public'])

filegroup(name = "macosgcc")

cc_toolchain_suite(
    name = "macos_homebrew_gcc",
    toolchains = {
         "darwin|compiler": ":custom_gcc_osx_toolchain",
    },
)

filegroup(name = "empty")
cc_toolchain(
    name = "custom_gcc_osx_toolchain",
    all_files = ":osx_wrapper",
    compiler_files = ":osx_wrapper",
    cpu = "darwin",
    dwp_files = ":empty",
    dynamic_runtime_libs = [":empty"],
    linker_files = ":osx_wrapper",
    objcopy_files = ":empty",
    static_runtime_libs = [":empty"],
    strip_files = ":empty",
    supports_param_files = 0,
)

filegroup(
    name = "osx_wrapper",
    srcs = ["osx_cc_wrapper.sh"],
)
