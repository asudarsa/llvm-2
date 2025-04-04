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

def arch_EQ : Joined<["--", "-"], "arch=">,
              Flags<[LinkerOnlyOption]>,
              MetaVarName<"<arch>">,
              HelpText<"The device architecture">;
def triple_EQ : Joined<["--", "-"], "triple=">,
                Flags<[LinkerOnlyOption]>,
                MetaVarName<"<triple>">,
                HelpText<"The device target triple">;

def save_temps : Flag<["--", "-"], "save-temps">,
  Flags<[LinkerOnlyOption]>, HelpText<"Save intermediate results">;

def rpath : Separate<["--", "-"], "rpath">;
def rpath_EQ : Joined<["--", "-"], "rpath=">, Flags<[HelpHidden]>, Alias<rpath>;

// Flags for the linker wrapper.
def linker_path_EQ : Joined<["--"], "linker-path=">,
                     MetaVarName<"<path>">,
                     HelpText<"The linker executable to invoke">;
def cuda_path_EQ : Joined<["--"], "cuda-path=">,
                   MetaVarName<"<dir>">,
                   HelpText<"Set the system CUDA path">;
def host_triple_EQ : Joined<["--"], "host-triple=">,
                     MetaVarName<"<triple>">,
                     HelpText<"Triple to use for the host compilation">;
def opt_level : Joined<["--"], "opt-level=">,
                MetaVarName<"<O0, O1, O2, or O3>">,
                HelpText<"Optimization level for LTO">;
def bitcode_library_EQ : Joined<["--"], "bitcode-library=">,
                         MetaVarName<"<kind>-<triple>-<arch>=<path>">,
                         HelpText<"Extra bitcode library to link">;
def builtin_bitcode_EQ
    : Joined<["--"], "builtin-bitcode=">,
      MetaVarName<"<triple>=<path>">,
      HelpText<
          "Perform a special internalizing link on the bitcode file. "
          "This is necessary for some vendor libraries to be linked correctly">;
def device_linker_args_EQ
    : Joined<["--"], "device-linker=">,
      MetaVarName<"<value> or <triple>=<value>">,
      HelpText<"Arguments to pass to the device linker invocation">;
def device_compiler_args_EQ
    : Joined<["--"], "device-compiler=">,
      MetaVarName<"<value> or <triple>=<value>">,
      HelpText<"Arguments to pass to the device compiler invocation">;
def clang_backend
    : Flag<["--"], "clang-backend">,
      HelpText<"Run the backend using clang rather than the LTO backend">;
def dry_run : Flag<["--"], "dry-run">,
              HelpText<"Print program arguments without running">;
def embed_bitcode : Flag<["--"], "embed-bitcode">,
                    HelpText<"Embed linked bitcode in the module">;
def debug : Flag<["--"], "device-debug">, HelpText<"Use debugging">;
def ptxas_arg : Joined<["--"], "ptxas-arg=">,
                HelpText<"Argument to pass to the 'ptxas' invocation">;
def compress : Flag<["--"], "compress">, HelpText<"Compress bundled files">;
def compression_level_eq : Joined<["--"], "compression-level=">,
                           HelpText<"Specify the compression level (integer)">;

// Flags passed to the device linker.
def whole_program : Flag<["--"], "whole-program">,
                    Flags<[HelpHidden]>,
                    HelpText<"LTO has visibility of all input files">;
def linker_arg_EQ : Joined<["--"], "linker-arg=">,
                    Flags<[HelpHidden]>,
                    HelpText<"An extra argument to be passed to the linker">;
def compiler_arg_EQ
    : Joined<["--"], "compiler-arg=">,
      Flags<[HelpHidden]>,
      HelpText<"An extra argument to be passed to the compiler">;

// Arguments for the LLVM backend.
def mllvm
    : Separate<["-"], "mllvm">,
      MetaVarName<"<arg>">,
      HelpText<
          "Arguments passed to LLVM, including Clang invocations, for which "
          "the '-mllvm' prefix is preserved. Use '-mllvm --help' for a list "
          "of options.">;
def offload_opt_eq_minus
    : Joined<["--", "-"], "offload-opt=">,
      Flags<[HelpHidden]>,
      HelpText<
          "Options passed to LLVM, not including the Clang invocation. Use "
          "'--offload-opt=--help' for a list of options.">;

// Standard linker flags also used by the linker wrapper.
def sysroot_EQ : Joined<["--"], "sysroot=">, HelpText<"Set the system root">;

def whole_archive : Flag<["--", "-"], "whole-archive">, Flags<[HelpHidden]>;
def no_whole_archive : Flag<["--", "-"], "no-whole-archive">, Flags<[HelpHidden]>;

def shared : Flag<["--", "-"], "shared">,
    HelpText<"Link device code to create a shared library">;

def library_path : JoinedOrSeparate<["-"], "L">,
                   MetaVarName<"<dir>">,
                   HelpText<"Add <dir> to the library search path">;
def library_path_EQ : Joined<["--", "-"], "library-path=">,
                      Flags<[HelpHidden]>,
                      Alias<library_path>;

// Options to specify SYCL device library files
def device_lib_EQ : CommaJoined<["--", "-"], "device-libraries=">,
                    HelpText<"A comma separated list of device libraries that "
                             "are linked during the device link.">;
def device_library_location_EQ
    : Joined<["--", "-"], "device-library-location=">,
      HelpText<"Location of SYCL device library files">;
def nvptx_device_lib_EQ
    : CommaJoined<["--", "-"], "nvptx-device-libraries=">,
      HelpText<"A comma separated list of nvptx-specific device libraries that "
               "are linked during the device link.">;

// Special option to pass in sycl-post-link options
def post_link_options_EQ
    : Joined<["--", "-"], "post-link-options=">,
      HelpText<"Options that will control sycl-post-link step">;

def module_split_mode_EQ
    : Joined<["--", "-"], "module-split-mode=">,
      HelpText<"Option that turns on split library with the given split mode">;

// TODO: Options will be removed when the sycl-post-link tool becomes removed.
def use_post_link_tool : Flag<["--", "-"], "use-post-link-tool">,
                         HelpText<"Use the sycl-post-link tool. On by default">;

def no_use_post_link_tool
    : Flag<["--", "-"], "no-use-post-link-tool">,
      HelpText<
          "Use a SYCL library instead of sycl-post-link tool. (experimental)">;

// Extra SYCL options to help generate sycl-post-link options that also depend
// on the target triple.
def remove_unused_external_funcs
    : Flag<["--", "-"], "remove-unused-external-funcs">,
      Flags<[HelpHidden]>;
def no_remove_unused_external_funcs
    : Flag<["--", "-"], "no-remove-unused-external-funcs">,
      Flags<[HelpHidden]>;
def device_code_split_esimd : Flag<["--", "-"], "device-code-split-esimd">,
                              Flags<[HelpHidden]>;
def no_device_code_split_esimd
    : Flag<["--", "-"], "no-device-code-split-esimd">,
      Flags<[HelpHidden]>;
def add_default_spec_consts_image
    : Flag<["--", "-"], "add-default-spec-consts-image">,
      Flags<[HelpHidden]>;
def no_add_default_spec_consts_image
    : Flag<["--", "-"], "no-add-default-spec-consts-image">,
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
def backend_compile_options_from_image_EQ
    : Joined<["--", "-"], "backend-compile-options-from-image=">,
      HelpText<"Compile options that will be transmitted to the SYCL backend "
               "compiler">;
def backend_link_options_from_image_EQ
    : Joined<["--", "-"], "backend-link-options-from-image=">,
      HelpText<
          "Link options that will be transmitted to the SYCL backend compiler">;

def thin_lto : Flag<["--", "-"], "thin-lto">,
               HelpText<"Link SYCL device code using thinLTO">;

def embed_ir : Flag<["--", "-"], "embed-ir">,
               HelpText<"Embed LLVM IR for runtime kernel fusion">;

// Options to enable/disable device dynamic linking.
def allow_device_image_dependencies
    : Flag<["--", "-"], "allow-device-image-dependencies">,
      Flags<[HelpHidden]>,
      HelpText<"Allow dependencies between device code images">;

def dump_device_code_EQ : Joined<["--", "-"], "dump-device-code=">,
                          HelpText<"Directory to dump offloading images to.">;

def llvm_spirv_path_EQ : Joined<["--"], "llvm-spirv-path=">,
                         Flags<[LinkerOnlyOption]>,
                         MetaVarName<"<dir>">,
                         HelpText<"Set the system llvm-spirv path">;

// Options to pass to llvm-spirv tool
def llvm_spirv_options_EQ
    : Joined<["--", "-"], "llvm-spirv-options=">,
      Flags<[LinkerOnlyOption]>,
      HelpText<"Options that will control llvm-spirv step">;
