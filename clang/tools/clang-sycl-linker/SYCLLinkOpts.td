include "llvm/Option/OptParser.td"

def LinkerOnlyOption : OptionFlag;

def help : Flag<["-", "--"], "help">,
  HelpText<"Display available options (--help-hidden for more)">;

def help_hidden : Flag<["-", "--"], "help-hidden">,
  HelpText<"Display all available options">;

def verbose : Flag<["-"], "v">, HelpText<"Print verbose information">;
def version : Flag<["--"], "version">,
  HelpText<"Display the version number and exit">;

def o : JoinedOrSeparate<["-"], "o">, MetaVarName<"<path>">,
  HelpText<"Path to file to write output">;
def output : Separate<["--"], "output-file">, Alias<o>, Flags<[HelpHidden]>,
  HelpText<"Alias for -o">;

def library_path_EQ : Joined<["--", "-"], "library-path=">,
  Flags<[HelpHidden]>, HelpText<"Add <dir> to the library search path">;

def device_libs_EQ : CommaJoined<["--", "-"], "device-libs=">,
  Flags<[LinkerOnlyOption]>,
  HelpText<"A comma separated list of device libraries that are linked during the device link.">;

def triple : Joined<["--"], "triple">,
  HelpText<"The device target triple">;
def arch : Separate<["--", "-"], "arch">,
  HelpText<"Specify the name of the target architecture.">;

def save_temps : Flag<["--", "-"], "save-temps">,
  Flags<[LinkerOnlyOption]>, HelpText<"Save intermediate results">;

def dry_run : Flag<["--", "-"], "dry-run">, Flags<[LinkerOnlyOption]>,
  HelpText<"Print generated commands without running.">;

def spirv_dump_device_code_EQ : Joined<["--", "-"], "spirv-dump-device-code=">,
  Flags<[LinkerOnlyOption]>,
  HelpText<"Path to the folder where the tool dumps SPIR-V device code. Other formats aren't dumped.">;

def is_windows_msvc_env : Flag<["--", "-"], "is-windows-msvc-env">,
  Flags<[LinkerOnlyOption, HelpHidden]>;

def llvm_spirv_path_EQ : Joined<["--"], "llvm-spirv-path=">,
  Flags<[LinkerOnlyOption]>, MetaVarName<"<dir>">,
  HelpText<"Set the system llvm-spirv path">;

// Options to pass to llvm-spirv tool
def llvm_spirv_options_EQ : Joined<["--", "-"], "llvm-spirv-options=">,
  Flags<[LinkerOnlyOption]>,
  HelpText<"Options that will control llvm-spirv step">;

def library_path : JoinedOrSeparate<["-"], "L">, MetaVarName<"<dir>">,
  HelpText<"Add <dir> to the library search path">;
def library_path_S : Separate<["--", "-"], "library-path">, Flags<[HelpHidden]>,
  Alias<library_path>;
def library_path_EQ : Joined<["--", "-"], "library-path=">, Flags<[HelpHidden]>,
  Alias<library_path>;

def library : JoinedOrSeparate<["-"], "l">, MetaVarName<"<libname>">,
  HelpText<"Search for library <libname>">;
def library_S : Separate<["--", "-"], "library">, Flags<[HelpHidden]>,
  Alias<library_path>;
def library_EQ : Joined<["--", "-"], "library=">, Flags<[HelpHidden]>,
  Alias<library_path>;

def rpath : Separate<["--", "-"], "rpath">;
def rpath_EQ : Joined<["--", "-"], "rpath=">, Flags<[HelpHidden]>, Alias<rpath>;

// Flags for the linker wrapper.
def linker_path_EQ : Joined<["--"], "linker-path=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<path>">,
  HelpText<"The linker executable to invoke">;
def cuda_path_EQ : Joined<["--"], "cuda-path=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<dir>">,
  HelpText<"Set the system CUDA path">;
def host_triple_EQ : Joined<["--"], "host-triple=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<triple>">,
  HelpText<"Triple to use for the host compilation">;
def opt_level : Joined<["--"], "opt-level=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<O0, O1, O2, or O3>">,
  HelpText<"Optimization level for LTO">;
def bitcode_library_EQ : Joined<["--"], "bitcode-library=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<kind>-<triple>-<arch>=<path>">,
  HelpText<"Extra bitcode library to link">;
def builtin_bitcode_EQ : Joined<["--"], "builtin-bitcode=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<triple>=<path>">,
  HelpText<"Perform a special internalizing link on the bitcode file. "
           "This is necessary for some vendor libraries to be linked correctly">;
def device_linker_args_EQ : Joined<["--"], "device-linker=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<value> or <triple>=<value>">,
  HelpText<"Arguments to pass to the device linker invocation">;
def device_compiler_args_EQ : Joined<["--"], "device-compiler=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<value> or <triple>=<value>">,
  HelpText<"Arguments to pass to the device compiler invocation">;
def clang_backend : Flag<["--"], "clang-backend">,
  Flags<[WrapperOnlyOption]>,
  HelpText<"Run the backend using clang rather than the LTO backend">;
def dry_run : Flag<["--"], "dry-run">,
  Flags<[WrapperOnlyOption]>,
  HelpText<"Print program arguments without running">;
def verbose : Flag<["--"], "wrapper-verbose">,
  Flags<[WrapperOnlyOption]>, HelpText<"Verbose output from tools">;
def embed_bitcode : Flag<["--"], "embed-bitcode">,
  Flags<[WrapperOnlyOption]>, HelpText<"Embed linked bitcode in the module">;
def debug : Flag<["--"], "device-debug">, Flags<[WrapperOnlyOption]>, 
  HelpText<"Use debugging">;
def ptxas_arg : Joined<["--"], "ptxas-arg=">,
  Flags<[WrapperOnlyOption]>,
  HelpText<"Argument to pass to the 'ptxas' invocation">;
def pass_remarks_EQ : Joined<["--"], "pass-remarks=">,
  Flags<[WrapperOnlyOption]>, HelpText<"Pass remarks for LTO">;
def pass_remarks_missed_EQ : Joined<["--"], "pass-remarks-missed=">,
  Flags<[WrapperOnlyOption]>, HelpText<"Pass remarks for LTO">;
def pass_remarks_analysis_EQ : Joined<["--"], "pass-remarks-analysis=">,
  Flags<[WrapperOnlyOption]>, HelpText<"Pass remarks for LTO">;
def print_wrapped_module : Flag<["--"], "print-wrapped-module">,
  Flags<[WrapperOnlyOption]>,
  HelpText<"Print the wrapped module's IR for testing">;
def save_temps : Flag<["--"], "save-temps">,
  Flags<[WrapperOnlyOption]>, HelpText<"Save intermediate results">;
def compress : Flag<["--"], "compress">,
  Flags<[WrapperOnlyOption]>, HelpText<"Compress bundled files">;
def compression_level_eq : Joined<["--"], "compression-level=">,
  Flags<[WrapperOnlyOption]>, HelpText<"Specify the compression level (integer)">;

def wrapper_time_trace_eq : Joined<["--"], "wrapper-time-trace=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<file>">,
  HelpText<"Enable time-trace and write the output to <file>">;
def wrapper_time_trace_granularity : Joined<["--"], "wrapper-time-trace-granularity=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<number>">,
  HelpText<"Set the granularity of time-trace updates">;

def wrapper_jobs : Joined<["--"], "wrapper-jobs=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<number>">,
  HelpText<"Sets the number of parallel jobs to use for device linking">;

def override_image : Joined<["--"], "override-image=">,
  Flags<[WrapperOnlyOption]>, MetaVarName<"<kind=file>">,
  HelpText<"Uses the provided file as if it were the output of the device link step">;

// Flags passed to the device linker.
def arch_EQ : Joined<["--"], "arch=">,
  Flags<[DeviceOnlyOption, HelpHidden]>, MetaVarName<"<arch>">,
  HelpText<"The device subarchitecture">;
def triple_EQ : Joined<["--"], "triple=">,
  Flags<[WrapperOnlyOption, DeviceOnlyOption, HelpHidden]>, MetaVarName<"<triple>">,
  HelpText<"The device target triple">;
def whole_program : Flag<["--"], "whole-program">,
  Flags<[DeviceOnlyOption, HelpHidden]>,
  HelpText<"LTO has visibility of all input files">;
def linker_arg_EQ : Joined<["--"], "linker-arg=">,
  Flags<[DeviceOnlyOption, HelpHidden]>,
  HelpText<"An extra argument to be passed to the linker">;
def compiler_arg_EQ : Joined<["--"], "compiler-arg=">,
  Flags<[DeviceOnlyOption, HelpHidden]>,
  HelpText<"An extra argument to be passed to the compiler">;

// Arguments for the LLVM backend.
def mllvm : Separate<["-"], "mllvm">, Flags<[WrapperOnlyOption]>,
  MetaVarName<"<arg>">,
  HelpText<"Arguments passed to LLVM, including Clang invocations, for which "
           "the '-mllvm' prefix is preserved. Use '-mllvm --help' for a list "
           "of options.">;
def offload_opt_eq_minus : Joined<["--", "-"], "offload-opt=">, Flags<[HelpHidden, WrapperOnlyOption]>,
  HelpText<"Options passed to LLVM, not including the Clang invocation. Use "
           "'--offload-opt=--help' for a list of options.">;

// Standard linker flags also used by the linker wrapper.
def sysroot_EQ : Joined<["--"], "sysroot=">, HelpText<"Set the system root">;

def o : JoinedOrSeparate<["-"], "o">, MetaVarName<"<path>">,
  HelpText<"Path to file to write output">;
def output_EQ : Joined<["--"], "output=">, Alias<o>, Flags<[HelpHidden]>,
  HelpText<"Alias for -o">;
def output : Separate<["--"], "output">, Alias<o>, Flags<[HelpHidden]>,
  HelpText<"Alias for -o">;

def library_path : JoinedOrSeparate<["-"], "L">, MetaVarName<"<dir>">,
  HelpText<"Add <dir> to the library search path">;
def library_path_S : Separate<["--", "-"], "library-path">, Flags<[HelpHidden]>,
  Alias<library_path>;
def library_path_EQ : Joined<["--", "-"], "library-path=">, Flags<[HelpHidden]>,
  Alias<library_path>;

def library : JoinedOrSeparate<["-"], "l">, MetaVarName<"<libname>">,
  HelpText<"Search for library <libname>">;
def library_S : Separate<["--", "-"], "library">, Flags<[HelpHidden]>,
  Alias<library_path>;
def library_EQ : Joined<["--", "-"], "library=">, Flags<[HelpHidden]>,
  Alias<library_path>;

def rpath : Separate<["--", "-"], "rpath">;
def rpath_EQ : Joined<["--", "-"], "rpath=">, Flags<[HelpHidden]>, Alias<rpath>;

def v : Flag<["--", "-"], "v">, HelpText<"Display the version number and exit">;
def version : Flag<["--", "-"], "version">, Flags<[HelpHidden]>, Alias<v>;

def whole_archive : Flag<["--", "-"], "whole-archive">, Flags<[HelpHidden]>;
def no_whole_archive : Flag<["--", "-"], "no-whole-archive">, Flags<[HelpHidden]>;

def shared : Flag<["--", "-"], "shared">,
    HelpText<"Link device code to create a shared library">;

// Options to specify SYCL device library files
def sycl_device_lib_EQ : CommaJoined<["--", "-"], "sycl-device-libraries=">,
  HelpText<"A comma separated list of device libraries that are linked during the device link.">;
def sycl_device_library_location_EQ : Joined<["--", "-"],
  "sycl-device-library-location=">,
  HelpText<"Location of SYCL device library files">;
def sycl_nvptx_device_lib_EQ : CommaJoined<["--", "-"], "sycl-nvptx-device-libraries=">,
  HelpText<"A comma separated list of nvptx-specific device libraries that are linked during the device link.">;

// Special option to pass in sycl-post-link options
def sycl_post_link_options_EQ : Joined<["--", "-"], "sycl-post-link-options=">,
  HelpText<"Options that will control sycl-post-link step">;

def sycl_module_split_mode_EQ :
  Joined<["--", "-"], "sycl-module-split-mode=">,
  HelpText<"Option that turns on split library with the given split mode">;

// TODO: Options will be removed when the sycl-post-link tool becomes removed.
def use_sycl_post_link_tool : Flag<["--", "-"], "use-sycl-post-link-tool">,
  HelpText<"Use the sycl-post-link tool. On by default">;

def no_use_sycl_post_link_tool : Flag<["--", "-"], "no-use-sycl-post-link-tool">,
  HelpText<"Use a SYCL library instead of sycl-post-link tool. (experimental)">;

// Extra SYCL options to help generate sycl-post-link options that also depend
// on the target triple.
def sycl_remove_unused_external_funcs : Flag<["--", "-"], "sycl-remove-unused-external-funcs">,
  Flags<[HelpHidden]>;
def no_sycl_remove_unused_external_funcs : Flag<["--", "-"], "no-sycl-remove-unused-external-funcs">,
  Flags<[HelpHidden]>;
def sycl_device_code_split_esimd : Flag<["--", "-"], "sycl-device-code-split-esimd">,
  Flags<[HelpHidden]>;
def no_sycl_device_code_split_esimd : Flag<["--", "-"], "no-sycl-device-code-split-esimd">,
  Flags<[HelpHidden]>;
def sycl_add_default_spec_consts_image : Flag<["--", "-"], "sycl-add-default-spec-consts-image">,
  Flags<[HelpHidden]>;
def no_sycl_add_default_spec_consts_image : Flag<["--", "-"], "no-sycl-add-default-spec-consts-image">,
  Flags<[HelpHidden]>;

// Special options to pass backend options required for AOT compilation
def gpu_tool_arg_EQ :
  Joined<["--", "-"], "gpu-tool-arg=">,
  HelpText<"Options that are passed to the backend of target device compiler for Intel GPU during AOT compilation">;
def cpu_tool_arg_EQ :
  Joined<["--", "-"], "cpu-tool-arg=">,
  HelpText<"Options that are passed to the backend of target device compiler for Intel CPU during AOT compilation">;

// Hidden options to store backend compile/link options that are stored in
// device images for SYCL offloading
def sycl_backend_compile_options_from_image_EQ : Joined<["--", "-"], "sycl-backend-compile-options-from-image=">,
  HelpText<"Compile options that will be transmitted to the SYCL backend compiler">;
def sycl_backend_link_options_from_image_EQ : Joined<["--", "-"], "sycl-backend-link-options-from-image=">,
  HelpText<"Link options that will be transmitted to the SYCL backend compiler">;

def sycl_thin_lto : Flag<["--", "-"], "sycl-thin-lto">,
HelpText<"Link SYCL device code using thinLTO">;

def sycl_embed_ir : Flag<["--", "-"], "sycl-embed-ir">,
HelpText<"Embed LLVM IR for runtime kernel fusion">;

// Options to enable/disable device dynamic linking.
def sycl_allow_device_image_dependencies : Flag<["--", "-"], "sycl-allow-device-image-dependencies">,
  Flags<[HelpHidden]>,
  HelpText<"Allow dependencies between device code images">;

debug
compress
compression_level_eq
embed_bitcode
clang_backend
builtin_bitcode_EQ
sycl_device_library_location_EQ
sycl_device_lib_EQ
sycl_post_link_options_EQ
ptxas_arg
host_triple_EQ
opt_level
offload_opt_eq_minus
mllvm
compiler_arg_EQ
linker_arg_EQ
whole_archive
no_whole_archive


