//=-------- clang-sycl-linker/ClangSYCLLinker.cpp - SYCL Linker util -------=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This tool executes a sequence of steps required to link device code in SYCL
// device images. SYCL device code linking requires a complex sequence of steps
// that include linking of llvm bitcode files, linking device library files
// with the fully linked source bitcode file(s), running several SYCL specific
// post-link steps on the fully linked bitcode file(s), and finally generating
// target-specific device code.
//===---------------------------------------------------------------------===//

#include "clang/Basic/Cuda.h"
#include "clang/Basic/Version.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Remarks/HotnessThresholdParser.h"
#include "llvm/SYCLPostLink/ModuleSplitter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/WithColor.h"
#include "llvm/TargetParser/Host.h"

using namespace llvm;
using namespace llvm::opt;
using namespace llvm::object;

/// Binary path for the CUDA installation.
static std::string CudaBinaryPath;

/// Save intermediary results.
static bool SaveTemps = false;

/// Print arguments without executing.
static bool DryRun = false;

/// Print verbose output.
static bool Verbose = false;

/// Filename of the output being created.
static StringRef OutputFile;

/// Directory to dump SPIR-V IR if requested by user.
static SmallString<128> SPIRVDumpDir;

static bool UseSYCLPostLinkTool;

static std::optional<llvm::module_split::IRSplitMode> SYCLModuleSplitMode;

static SmallString<128> OffloadImageDumpDir;

static void printVersion(raw_ostream &OS) {
  OS << clang::getClangToolFullVersion("clang-sycl-linker") << '\n';
}

/// The value of `argv[0]` when run.
static const char *Executable;

/// Mutex lock to protect writes to shared TempFiles in parallel.
static std::mutex TempFilesMutex;

/// Temporary files to be cleaned up.
static SmallVector<SmallString<128>> TempFiles;

namespace {
// Must not overlap with llvm::opt::DriverFlag.
enum LinkerFlags { LinkerOnlyOption = (1 << 4) };

enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "SYCLLinkOpts.inc"
  LastOption
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "SYCLLinkOpts.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "SYCLLinkOpts.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

static constexpr OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "SYCLLinkOpts.inc"
#undef OPTION
};

class LinkerOptTable : public opt::GenericOptTable {
public:
  LinkerOptTable()
      : opt::GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {}
};

const OptTable &getOptTable() {
  static const LinkerOptTable *Table = []() {
    auto Result = std::make_unique<LinkerOptTable>();
    return Result.release();
  }();
  return *Table;
}

[[noreturn]] void reportError(Error E) {
  outs().flush();
  logAllUnhandledErrors(std::move(E), WithColor::error(errs(), Executable));
  exit(EXIT_FAILURE);
}

std::string getMainExecutable(const char *Name) {
  void *Ptr = (void *)(intptr_t)&getMainExecutable;
  auto COWPath = sys::fs::getMainExecutable(Name, Ptr);
  return sys::path::parent_path(COWPath).str();
}

/// Get a temporary filename suitable for output.
Expected<StringRef> createOutputFile(const Twine &Prefix, StringRef Extension) {
  std::scoped_lock<decltype(TempFilesMutex)> Lock(TempFilesMutex);
  SmallString<128> OutputFile;
  if (SaveTemps) {
    // Generate a unique path name without creating a file
    sys::fs::createUniquePath(Prefix + "-%%%%%%." + Extension, OutputFile,
                              /*MakeAbsolute=*/false);
  } else {
    if (std::error_code EC =
            sys::fs::createTemporaryFile(Prefix, Extension, OutputFile))
      return createFileError(OutputFile, EC);
  }

  TempFiles.emplace_back(std::move(OutputFile));
  return TempFiles.back();
}

Expected<StringRef> createTempFile(const ArgList &Args, const Twine &Prefix,
                                   StringRef Extension) {
  SmallString<128> OutputFile;
  if (Args.hasArg(OPT_save_temps)) {
    // Generate a unique path name without creating a file
    sys::fs::createUniquePath(Prefix + "-%%%%%%." + Extension, OutputFile,
                              /*MakeAbsolute=*/false);
  } else {
    if (std::error_code EC =
            sys::fs::createTemporaryFile(Prefix, Extension, OutputFile))
      return createFileError(OutputFile, EC);
  }

  TempFiles.emplace_back(std::move(OutputFile));
  return TempFiles.back();
}

Expected<std::string> findProgram(const ArgList &Args, StringRef Name,
                                  ArrayRef<StringRef> Paths) {
  if (Args.hasArg(OPT_dry_run))
    return Name.str();
  ErrorOr<std::string> Path = sys::findProgramByName(Name, Paths);
  if (!Path)
    Path = sys::findProgramByName(Name);
  if (!Path)
    return createStringError(Path.getError(),
                             "Unable to find '" + Name + "' in path");
  return *Path;
}

bool linkerSupportsLTO(const ArgList &Args) {
  llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  return Triple.isNVPTX() || Triple.isAMDGPU() ||
         Args.getLastArgValue(OPT_linker_path_EQ).ends_with("lld");
}

void printCommands(ArrayRef<StringRef> CmdArgs) {
  if (CmdArgs.empty())
    return;

  llvm::errs() << " \"" << CmdArgs.front() << "\" ";
  llvm::errs() << llvm::join(std::next(CmdArgs.begin()), CmdArgs.end(), " ")
               << "\n";
}

/// Execute the command \p ExecutablePath with the arguments \p Args.
Error executeCommands(StringRef ExecutablePath, ArrayRef<StringRef> Args) {
  if (Verbose || DryRun)
    printCommands(Args);

  if (!DryRun)
    if (sys::ExecuteAndWait(ExecutablePath, Args))
      return createStringError(
          "'%s' failed", sys::path::filename(ExecutablePath).str().c_str());
  return Error::success();
}

// end namespace

namespace nvptx {
Expected<StringRef>
fatbinary(ArrayRef<std::pair<StringRef, StringRef>> InputFiles,
          const ArgList &Args) {
  llvm::TimeTraceScope TimeScope("NVPTX fatbinary");
  // NVPTX uses the fatbinary program to bundle the linked images.
  Expected<std::string> FatBinaryPath =
      findProgram(Args, "fatbinary", {CudaBinaryPath + "/bin"});
  if (!FatBinaryPath)
    return FatBinaryPath.takeError();

  llvm::Triple Triple(
      Args.getLastArgValue(OPT_host_triple_EQ, sys::getDefaultTargetTriple()));

  // Create a new file to write the linked device image to.
  auto TempFileOrErr =
      createOutputFile(sys::path::filename(OutputFile), "fatbin");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  SmallVector<StringRef, 16> CmdArgs;
  CmdArgs.push_back(*FatBinaryPath);
  CmdArgs.push_back(Triple.isArch64Bit() ? "-64" : "-32");
  CmdArgs.push_back("--create");
  CmdArgs.push_back(*TempFileOrErr);
  for (const auto &[File, Arch] : InputFiles)
    CmdArgs.push_back(
        Args.MakeArgString("--image=profile=" + Arch + ",file=" + File));

  if (Error Err = executeCommands(*FatBinaryPath, CmdArgs))
    return std::move(Err);

  return *TempFileOrErr;
}

// ptxas binary
Expected<StringRef> ptxas(StringRef InputFile, const ArgList &Args,
                          StringRef Arch) {
  llvm::TimeTraceScope TimeScope("NVPTX ptxas");
  // NVPTX uses the ptxas program to process assembly files.
  Expected<std::string> PtxasPath =
      findProgram(Args, "ptxas", {CudaBinaryPath + "/bin"});
  if (!PtxasPath)
    return PtxasPath.takeError();

  llvm::Triple Triple(
      Args.getLastArgValue(OPT_host_triple_EQ, sys::getDefaultTargetTriple()));

  // Create a new file to write the output to.
  auto TempFileOrErr =
      createOutputFile(sys::path::filename(OutputFile), "cubin");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  SmallVector<StringRef, 16> CmdArgs;
  CmdArgs.push_back(*PtxasPath);
  CmdArgs.push_back(Triple.isArch64Bit() ? "-m64" : "-m32");
  // Pass -v to ptxas if it was passed to the driver.
  if (Args.hasArg(OPT_verbose))
    CmdArgs.push_back("-v");
  StringRef OptLevel = Args.getLastArgValue(OPT_opt_level, "O2");
  if (Args.hasArg(OPT_debug))
    CmdArgs.push_back("-g");
  else
    CmdArgs.push_back(Args.MakeArgString("-" + OptLevel));
  CmdArgs.push_back("--gpu-name");
  CmdArgs.push_back(Arch);
  CmdArgs.push_back("--output-file");
  CmdArgs.push_back(*TempFileOrErr);
  CmdArgs.push_back(InputFile);
  if (Error Err = executeCommands(*PtxasPath, CmdArgs))
    return std::move(Err);
  return *TempFileOrErr;
}
} // namespace nvptx

namespace amdgcn {
Expected<StringRef>
fatbinary(ArrayRef<std::pair<StringRef, StringRef>> InputFiles,
          const ArgList &Args) {
  llvm::TimeTraceScope TimeScope("AMDGPU Fatbinary");

  // AMDGPU uses the clang-offload-bundler to bundle the linked images.
  Expected<std::string> OffloadBundlerPath =
      findProgram(Args, "clang-offload-bundler",
                  {getMainExecutable("clang-offload-bundler")});
  if (!OffloadBundlerPath)
    return OffloadBundlerPath.takeError();

  llvm::Triple Triple(
      Args.getLastArgValue(OPT_host_triple_EQ, sys::getDefaultTargetTriple()));

  // Create a new file to write the linked device image to.
  auto TempFileOrErr =
      createOutputFile(sys::path::filename(OutputFile), "hipfb");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  BumpPtrAllocator Alloc;
  StringSaver Saver(Alloc);

  SmallVector<StringRef, 16> CmdArgs;
  CmdArgs.push_back(*OffloadBundlerPath);
  CmdArgs.push_back("-type=o");
  CmdArgs.push_back("-bundle-align=4096");

  if (Args.hasArg(OPT_compress))
    CmdArgs.push_back("-compress");
  if (auto *Arg = Args.getLastArg(OPT_compression_level_eq))
    CmdArgs.push_back(
        Args.MakeArgString(Twine("-compression-level=") + Arg->getValue()));

  SmallVector<StringRef> Targets = {"-targets=host-x86_64-unknown-linux-gnu"};
  for (const auto &[File, Arch] : InputFiles)
    Targets.push_back(Saver.save("hip-amdgcn-amd-amdhsa--" + Arch));
  CmdArgs.push_back(Saver.save(llvm::join(Targets, ",")));

#ifdef _WIN32
  CmdArgs.push_back("-input=NUL");
#else
  CmdArgs.push_back("-input=/dev/null");
#endif
  for (const auto &[File, Arch] : InputFiles)
    CmdArgs.push_back(Saver.save("-input=" + File));

  CmdArgs.push_back(Saver.save("-output=" + *TempFileOrErr));

  if (Error Err = executeCommands(*OffloadBundlerPath, CmdArgs))
    return std::move(Err);

  return *TempFileOrErr;
}
} // namespace amdgcn

namespace generic {
Expected<StringRef> clang(ArrayRef<StringRef> InputFiles, const ArgList &Args) {
  llvm::TimeTraceScope TimeScope("Clang");
  // Use `clang` to invoke the appropriate device tools.
  Expected<std::string> ClangPath =
      findProgram(Args, "clang", {getMainExecutable("clang")});
  if (!ClangPath)
    return ClangPath.takeError();

  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  StringRef Arch = Args.getLastArgValue(OPT_arch_EQ);
  if (Arch.empty())
    Arch = "native";
  // Create a new file to write the linked device image to. Assume that the
  // input filename already has the device and architecture.
  auto TempFileOrErr = createOutputFile(sys::path::filename(OutputFile) + "." +
                                            Triple.getArchName() + "." + Arch,
                                        "img");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  StringRef OptLevel = Args.getLastArgValue(OPT_opt_level, "O2");
  SmallVector<StringRef, 16> CmdArgs{
      *ClangPath,
      "--no-default-config",
      "-o",
      *TempFileOrErr,
      Args.MakeArgString("--target=" + Triple.getTriple()),
      Triple.isAMDGPU() ? Args.MakeArgString("-mcpu=" + Arch)
                        : Args.MakeArgString("-march=" + Arch),
      Args.MakeArgString("-" + OptLevel),
  };

  // Forward all of the `--offload-opt` and similar options to the device.
  CmdArgs.push_back("-flto");
  for (auto &Arg : Args.filtered(OPT_offload_opt_eq_minus, OPT_mllvm))
    CmdArgs.append(
        {"-Xlinker",
         Args.MakeArgString("--plugin-opt=" + StringRef(Arg->getValue()))});

  if (!Triple.isNVPTX() && !Triple.isSPIRV())
    CmdArgs.push_back("-Wl,--no-undefined");

  if (Triple.isNVPTX())
    CmdArgs.push_back("-Wl,--lto-emit-asm");
  for (StringRef InputFile : InputFiles)
    CmdArgs.push_back(InputFile);

  // If this is CPU offloading we copy the input libraries.
  if (!Triple.isAMDGPU() && !Triple.isNVPTX() && !Triple.isSPIRV()) {
    CmdArgs.push_back("-Wl,-Bsymbolic");
    CmdArgs.push_back("-shared");
    ArgStringList LinkerArgs;
    for (const opt::Arg *Arg :
         Args.filtered(OPT_INPUT, OPT_library_path_EQ, OPT_rpath,
                       OPT_whole_archive, OPT_no_whole_archive)) {
      // Sometimes needed libraries are passed by name, such as when using
      // sanitizers. We need to check the file magic for any libraries.
      if (Arg->getOption().matches(OPT_INPUT)) {
        if (!sys::fs::exists(Arg->getValue()) ||
            sys::fs::is_directory(Arg->getValue()))
          continue;

        file_magic Magic;
        if (auto EC = identify_magic(Arg->getValue(), Magic))
          return createStringError("Failed to open %s", Arg->getValue());
        if (Magic != file_magic::archive &&
            Magic != file_magic::elf_shared_object)
          continue;
      }
      if (Arg->getOption().matches(OPT_whole_archive))
        LinkerArgs.push_back(Args.MakeArgString("-Wl,--whole-archive"));
      else if (Arg->getOption().matches(OPT_no_whole_archive))
        LinkerArgs.push_back(Args.MakeArgString("-Wl,--no-whole-archive"));
      else
        Arg->render(Args, LinkerArgs);
    }
    llvm::copy(LinkerArgs, std::back_inserter(CmdArgs));
  }

  // Pass on -mllvm options to the linker invocation.
  for (const opt::Arg *Arg : Args.filtered(OPT_mllvm))
    CmdArgs.append({"-Xlinker", Args.MakeArgString(
                                    "-mllvm=" + StringRef(Arg->getValue()))});

  if (Args.hasArg(OPT_debug))
    CmdArgs.push_back("-g");

  if (SaveTemps)
    CmdArgs.push_back("-save-temps");

  if (SaveTemps && linkerSupportsLTO(Args))
    CmdArgs.push_back("-Wl,--save-temps");

  if (Args.hasArg(OPT_embed_bitcode))
    CmdArgs.push_back("-Wl,--lto-emit-llvm");

  if (Verbose)
    CmdArgs.push_back("-v");

  if (!CudaBinaryPath.empty())
    CmdArgs.push_back(Args.MakeArgString("--cuda-path=" + CudaBinaryPath));

  for (StringRef Arg : Args.getAllArgValues(OPT_ptxas_arg))
    llvm::copy(
        SmallVector<StringRef>({"-Xcuda-ptxas", Args.MakeArgString(Arg)}),
        std::back_inserter(CmdArgs));

  for (StringRef Arg : Args.getAllArgValues(OPT_linker_arg_EQ))
    CmdArgs.append({"-Xlinker", Args.MakeArgString(Arg)});
  for (StringRef Arg : Args.getAllArgValues(OPT_compiler_arg_EQ))
    CmdArgs.push_back(Args.MakeArgString(Arg));

  for (StringRef Arg : Args.getAllArgValues(OPT_builtin_bitcode_EQ)) {
    if (llvm::Triple(Arg.split('=').first) == Triple)
      CmdArgs.append({"-Xclang", "-mlink-builtin-bitcode", "-Xclang",
                      Args.MakeArgString(Arg.split('=').second)});
  }

  // The OpenMPOpt pass can introduce new calls and is expensive, we do
  // not want this when running CodeGen through clang.
  if (Args.hasArg(OPT_clang_backend) || Args.hasArg(OPT_builtin_bitcode_EQ))
    CmdArgs.append({"-mllvm", "-openmp-opt-disable"});

  if (Error Err = executeCommands(*ClangPath, CmdArgs))
    return std::move(Err);

  return *TempFileOrErr;
}
} // namespace generic

Expected<StringRef> writeOffloadFile(const OffloadFile &File) {
  const OffloadBinary &Binary = *File.getBinary();

  StringRef Prefix =
      sys::path::stem(Binary.getMemoryBufferRef().getBufferIdentifier());
  StringRef Suffix = getImageKindName(Binary.getImageKind());

  auto TempFileOrErr = createOutputFile(
      Prefix + "-" + Binary.getTriple() + "-" + Binary.getArch(), Suffix);
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  Expected<std::unique_ptr<FileOutputBuffer>> OutputOrErr =
      FileOutputBuffer::create(*TempFileOrErr, Binary.getImage().size());
  if (!OutputOrErr)
    return OutputOrErr.takeError();
  std::unique_ptr<FileOutputBuffer> Output = std::move(*OutputOrErr);
  llvm::copy(Binary.getImage(), Output->getBufferStart());
  if (Error E = Output->commit())
    return std::move(E);

  return *TempFileOrErr;
}

/// This routine is used to convert SPIR-V input files into LLVM IR files.
/// 'llvm-spirv -r' command is used for this purpose.
/// If input is not a SPIR-V file, then the original file is returned.
/// TODO: Add a check to identify SPIR-V files and exit early if the input is
/// not a SPIR-V file.
/// 'Filename' is the input file that could be a SPIR-V file.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// llvm-spirv tool.
static Expected<StringRef> convertSPIRVToIR(StringRef Filename,
                                            const ArgList &Args) {
  Expected<std::string> SPIRVToIRWrapperPath = findProgram(
      Args, "spirv-to-ir-wrapper", {getMainExecutable("spirv-to-ir-wrapper")});
  if (!SPIRVToIRWrapperPath)
    return SPIRVToIRWrapperPath.takeError();

  // Create a new file to write the converted file to.
  auto TempFileOrErr = createOutputFile(sys::path::filename(OutputFile), "bc");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  SmallVector<StringRef, 8> CmdArgs;
  CmdArgs.push_back(*SPIRVToIRWrapperPath);
  CmdArgs.push_back(Filename);
  CmdArgs.push_back("-o");
  CmdArgs.push_back(*TempFileOrErr);
  CmdArgs.push_back("--llvm-spirv-opts");
  CmdArgs.push_back("--spirv-preserve-auxdata --spirv-target-env=SPV-IR "
                    "--spirv-builtin-format=global");
  if (Error Err = executeCommands(*SPIRVToIRWrapperPath, CmdArgs))
    return std::move(Err);
  return *TempFileOrErr;
}

/// Link all SYCL device input files into one before adding device library
/// files. Device linking is performed using llvm-link tool.
/// 'InputFiles' is the list of all LLVM IR device input files.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// llvm-link tool.
Expected<StringRef> linkDeviceInputFiles(SmallVectorImpl<StringRef> &InputFiles,
                                         const ArgList &Args) {
  llvm::TimeTraceScope TimeScope("SYCL LinkDeviceInputFiles");

  Expected<std::string> LLVMLinkPath =
      findProgram(Args, "llvm-link", {getMainExecutable("llvm-link")});
  if (!LLVMLinkPath)
    return LLVMLinkPath.takeError();

  // Create a new file to write the linked device file to.
  auto OutFileOrErr = createOutputFile(sys::path::filename(OutputFile), "bc");
  if (!OutFileOrErr)
    return OutFileOrErr.takeError();

  SmallVector<StringRef, 8> CmdArgs;
  CmdArgs.push_back(*LLVMLinkPath);
  for (auto &File : InputFiles) {
    auto IRFile = convertSPIRVToIR(File, Args);
    if (!IRFile)
      return IRFile.takeError();
    CmdArgs.push_back(*IRFile);
  }
  CmdArgs.push_back("-o");
  CmdArgs.push_back(*OutFileOrErr);
  CmdArgs.push_back("--suppress-warnings");
  if (Error Err = executeCommands(*LLVMLinkPath, CmdArgs))
    return std::move(Err);
  return *OutFileOrErr;
}

// This utility function is used to gather all SYCL device library files that
// will be linked with input device files.
// The list of files and its location are passed from driver.
static Error getSYCLDeviceLibs(SmallVector<std::string, 16> &DeviceLibFiles,
                               const ArgList &Args) {
  StringRef SYCLDeviceLibLoc("");
  if (Arg *A = Args.getLastArg(OPT_sycl_device_library_location_EQ))
    SYCLDeviceLibLoc = A->getValue();
  if (Arg *A = Args.getLastArg(OPT_sycl_device_lib_EQ)) {
    if (A->getValues().size() == 0)
      return createStringError(
          inconvertibleErrorCode(),
          "Number of device library files cannot be zero.");
    for (StringRef Val : A->getValues()) {
      SmallString<128> LibName(SYCLDeviceLibLoc);
      llvm::sys::path::append(LibName, Val);
      if (llvm::sys::fs::exists(LibName))
        DeviceLibFiles.push_back(std::string(LibName));
      else
        return createStringError(inconvertibleErrorCode(),
                                 std::string(LibName) +
                                     " SYCL device library file is not found.");
    }
  }
  return Error::success();
}

/// Link all device library files and input file into one LLVM IR file. This
/// linking is performed using llvm-link tool.
/// 'InputFiles' is the list of all LLVM IR device input files.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// llvm-link tool.
static Expected<StringRef>
linkDeviceLibFiles(SmallVectorImpl<StringRef> &InputFiles,
                   const ArgList &Args) {
  llvm::TimeTraceScope TimeScope("LinkDeviceLibraryFiles");

  Expected<std::string> LLVMLinkPath =
      findProgram(Args, "llvm-link", {getMainExecutable("llvm-link")});
  if (!LLVMLinkPath)
    return LLVMLinkPath.takeError();

  // Create a new file to write the linked device file to.
  auto OutFileOrErr = createOutputFile(sys::path::filename(OutputFile), "bc");
  if (!OutFileOrErr)
    return OutFileOrErr.takeError();

  SmallVector<StringRef, 8> CmdArgs;
  CmdArgs.push_back(*LLVMLinkPath);
  CmdArgs.push_back("-only-needed");
  for (auto &File : InputFiles)
    CmdArgs.push_back(File);
  CmdArgs.push_back("-o");
  CmdArgs.push_back(*OutFileOrErr);
  CmdArgs.push_back("--suppress-warnings");
  if (Error Err = executeCommands(*LLVMLinkPath, CmdArgs))
    return std::move(Err);
  return *OutFileOrErr;
}

/// This function is used to link all SYCL device input files into a single
/// LLVM IR file. This file is in turn linked with all SYCL device library
/// files.
/// 'InputFiles' is the list of all LLVM IR device input files.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// llvm-link tool.
static Expected<StringRef> linkDeviceBitcode(ArrayRef<StringRef> InputFiles,
                                             const ArgList &Args) {
  SmallVector<StringRef, 16> InputFilesVec;
  for (StringRef InputFile : InputFiles)
    InputFilesVec.emplace_back(InputFile);
  // First llvm-link step.
  auto LinkedFile = linkDeviceInputFiles(InputFilesVec, Args);
  if (!LinkedFile)
    reportError(LinkedFile.takeError());

  InputFilesVec.clear();
  InputFilesVec.emplace_back(*LinkedFile);

  // Gathering device library files
  SmallVector<std::string, 16> DeviceLibFiles;
  if (Error Err = getSYCLDeviceLibs(DeviceLibFiles, Args))
    reportError(std::move(Err));
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  SmallVector<std::string, 16> ExtractedDeviceLibFiles;
  for (auto &File : DeviceLibFiles) {
    auto BufferOrErr = MemoryBuffer::getFile(File);
    if (!BufferOrErr)
      return createFileError(File, BufferOrErr.getError());
    auto Buffer = std::move(*BufferOrErr);
    SmallVector<OffloadFile> Binaries;
    if (Error Err = extractOffloadBinaries(Buffer->getMemBufferRef(), Binaries))
      return std::move(Err);
    bool CompatibleBinaryFound = false;
    for (auto &Binary : Binaries) {
      auto BinTriple = Binary.getBinary()->getTriple();
      if (BinTriple == Triple.getTriple()) {
        auto FileNameOrErr =
            writeOffloadFile(Binary);
        if (!FileNameOrErr)
          return FileNameOrErr.takeError();
        ExtractedDeviceLibFiles.emplace_back(*FileNameOrErr);
        CompatibleBinaryFound = true;
      }
    }
    if (!CompatibleBinaryFound)
      WithColor::warning(errs(), Executable)
          << "Compatible SYCL device library binary not found\n";
  }

  // For NVPTX backend we need to also link libclc and CUDA libdevice.
  if (Triple.isNVPTX()) {
    if (Arg *A = Args.getLastArg(OPT_sycl_nvptx_device_lib_EQ)) {
      if (A->getValues().size() == 0)
        return createStringError(
            inconvertibleErrorCode(),
            "Number of device library files cannot be zero.");
      for (StringRef Val : A->getValues()) {
        SmallString<128> LibName(Val);
        if (llvm::sys::fs::exists(LibName))
          ExtractedDeviceLibFiles.emplace_back(std::string(LibName));
        else
          return createStringError(
              inconvertibleErrorCode(),
              std::string(LibName) +
                  " SYCL device library file for NVPTX is not found.");
      }
    }
  }

  // Make sure that SYCL device library files are available.
  // Note: For AMD targets, we do not pass any SYCL device libraries.
  if (ExtractedDeviceLibFiles.empty()) {
    // TODO: Add NVPTX when ready
    if (Triple.isSPIROrSPIRV())
      return createStringError(
          inconvertibleErrorCode(),
          " SYCL device library file list cannot be empty.");
    return *LinkedFile;
  }

  for (auto &File : ExtractedDeviceLibFiles)
    InputFilesVec.emplace_back(File);
  // second llvm-link step
  auto DeviceLinkedFile = linkDeviceLibFiles(InputFilesVec, Args);
  if (!DeviceLinkedFile)
    reportError(DeviceLinkedFile.takeError());

  return *DeviceLinkedFile;
}

/// Add any sycl-post-link options that rely on a specific Triple in addition
/// to user supplied options.
/// NOTE: Any changes made here should be reflected in the similarly named
/// function in clang/lib/Driver/ToolChains/Clang.cpp.
static void
getTripleBasedSYCLPostLinkOpts(const ArgList &Args,
                               SmallVector<StringRef, 8> &PostLinkArgs,
                               const llvm::Triple Triple) {
  const llvm::Triple HostTriple(Args.getLastArgValue(OPT_host_triple_EQ));
  bool SYCLNativeCPU = (HostTriple == Triple);
  bool SpecConstsSupported = (!Triple.isNVPTX() && !Triple.isAMDGCN() &&
                              !Triple.isSPIRAOT() && !SYCLNativeCPU);
  if (SpecConstsSupported)
    PostLinkArgs.push_back("-spec-const=native");
  else
    PostLinkArgs.push_back("-spec-const=emulation");

  // TODO: If we ever pass -ir-output-only based on the triple,
  // make sure we don't pass -properties.
  PostLinkArgs.push_back("-properties");

  // See if device code splitting is already requested. If not requested, then
  // set -split=auto for non-FPGA targets.
  bool NoSplit = true;
  for (auto Arg : PostLinkArgs)
    if (Arg.contains("-split=")) {
      NoSplit = false;
      break;
    }
  if (NoSplit && (Triple.getSubArch() != llvm::Triple::SPIRSubArch_fpga))
    PostLinkArgs.push_back("-split=auto");

  // On Intel targets we don't need non-kernel functions as entry points,
  // because it only increases amount of code for device compiler to handle,
  // without any actual benefits.
  // TODO: Try to extend this feature for non-Intel GPUs.
  if ((!Args.hasFlag(OPT_no_sycl_remove_unused_external_funcs,
                     OPT_sycl_remove_unused_external_funcs, false) &&
       !SYCLNativeCPU) &&
      !Args.hasArg(OPT_sycl_allow_device_image_dependencies) &&
      !Triple.isNVPTX() && !Triple.isAMDGPU())
    PostLinkArgs.push_back("-emit-only-kernels-as-entry-points");

  if (!Triple.isAMDGCN())
    PostLinkArgs.push_back("-emit-param-info");
  // Enable program metadata
  if (Triple.isNVPTX() || Triple.isAMDGCN() || SYCLNativeCPU)
    PostLinkArgs.push_back("-emit-program-metadata");

  bool SplitEsimdByDefault = Triple.isSPIROrSPIRV();
  bool SplitEsimd =
      Args.hasFlag(OPT_sycl_device_code_split_esimd,
                   OPT_no_sycl_device_code_split_esimd, SplitEsimdByDefault);
  if (!Args.hasArg(OPT_sycl_thin_lto))
    PostLinkArgs.push_back("-symbols");
  // Specialization constant info generation is mandatory -
  // add options unconditionally
  PostLinkArgs.push_back("-emit-exported-symbols");
  PostLinkArgs.push_back("-emit-imported-symbols");
  if (SplitEsimd)
    PostLinkArgs.push_back("-split-esimd");
  PostLinkArgs.push_back("-lower-esimd");

  bool IsAOT = Triple.isNVPTX() || Triple.isAMDGCN() || Triple.isSPIRAOT();
  if (Args.hasFlag(OPT_sycl_add_default_spec_consts_image,
                   OPT_no_sycl_add_default_spec_consts_image, false) &&
      IsAOT)
    PostLinkArgs.push_back("-generate-device-image-default-spec-consts");
}

/// Run sycl-post-link tool for SYCL offloading.
/// 'InputFiles' is the list of input LLVM IR files.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// sycl-post-link tool.
static Expected<std::vector<module_split::SplitModule>>
runSYCLPostLinkTool(ArrayRef<StringRef> InputFiles, const ArgList &Args) {
  Expected<std::string> SYCLPostLinkPath = findProgram(
      Args, "sycl-post-link", {getMainExecutable("sycl-post-link")});
  if (!SYCLPostLinkPath)
    return SYCLPostLinkPath.takeError();

  // Create a new file to write the output of sycl-post-link to.
  auto TempFileOrErr =
      createOutputFile(sys::path::filename(OutputFile), "table");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  SmallVector<StringRef, 8> CmdArgs;
  CmdArgs.push_back(*SYCLPostLinkPath);
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  Arg *SYCLDeviceLibLoc = Args.getLastArg(OPT_sycl_device_library_location_EQ);
  if (SYCLDeviceLibLoc && !Triple.isSPIRAOT()) {
    std::string SYCLDeviceLibLocParam = SYCLDeviceLibLoc->getValue();
    std::string BF16DeviceLibLoc =
        SYCLDeviceLibLocParam + "/libsycl-native-bfloat16.bc";
    if (llvm::sys::fs::exists(BF16DeviceLibLoc)) {
      SYCLDeviceLibLocParam = "--device-lib-dir=" + SYCLDeviceLibLocParam;
      CmdArgs.push_back(Args.MakeArgString(StringRef(SYCLDeviceLibLocParam)));
    }
  }
  getTripleBasedSYCLPostLinkOpts(Args, CmdArgs, Triple);
  StringRef SYCLPostLinkOptions;
  if (Arg *A = Args.getLastArg(OPT_sycl_post_link_options_EQ))
    SYCLPostLinkOptions = A->getValue();
  SYCLPostLinkOptions.split(CmdArgs, " ", /* MaxSplit = */ -1,
                            /* KeepEmpty = */ false);
  CmdArgs.push_back("-o");
  CmdArgs.push_back(*TempFileOrErr);
  for (auto &File : InputFiles)
    CmdArgs.push_back(File);
  if (Error Err = executeCommands(*SYCLPostLinkPath, CmdArgs))
    return std::move(Err);

  if (DryRun) {
    // In DryRun we need a dummy entry in order to continue the whole pipeline.
    auto ImageFileOrErr = createOutputFile(
        sys::path::filename(OutputFile) + ".sycl.split.image", "bc");
    if (!ImageFileOrErr)
      return ImageFileOrErr.takeError();

    std::vector Modules = {module_split::SplitModule(
        *ImageFileOrErr, util::PropertySetRegistry(), "")};
    return Modules;
  }

  return llvm::module_split::parseSplitModulesFromFile(*TempFileOrErr);
}

/// Invokes SYCL Split library for SYCL offloading.
///
/// \param InputFiles the list of input LLVM IR files.
/// \param Args Encompasses all arguments for linking and wrapping device code.
///  It will be parsed to generate options required to be passed to SYCL split
///  library.
/// \param Mode The splitting mode.
/// \returns The vector of split modules.
static Expected<std::vector<module_split::SplitModule>>
runSYCLSplitLibrary(ArrayRef<StringRef> InputFiles, const ArgList &Args,
                    module_split::IRSplitMode Mode) {
  std::vector<module_split::SplitModule> SplitModules;
  if (DryRun) {
    auto OutputFileOrErr = createOutputFile(
        sys::path::filename(OutputFile) + ".sycl.split.image", "bc");
    if (!OutputFileOrErr)
      return OutputFileOrErr.takeError();

    StringRef OutputFilePath = *OutputFileOrErr;
    auto InputFilesStr = llvm::join(InputFiles.begin(), InputFiles.end(), ",");
    errs() << formatv("sycl-module-split: input: {0}, output: {1}\n",
                      InputFilesStr, OutputFilePath);
    SplitModules.emplace_back(OutputFilePath, util::PropertySetRegistry(), "");
    return SplitModules;
  }

  llvm::module_split::ModuleSplitterSettings Settings;
  Settings.Mode = Mode;
  Settings.OutputPrefix = "";

  for (StringRef InputFile : InputFiles) {
    SMDiagnostic Err;
    LLVMContext C;
    std::unique_ptr<Module> M = parseIRFile(InputFile, Err, C);
    if (!M)
      return createStringError(inconvertibleErrorCode(), Err.getMessage());

    auto SplitModulesOrErr =
        module_split::splitSYCLModule(std::move(M), Settings);
    if (!SplitModulesOrErr)
      return SplitModulesOrErr.takeError();

    auto &NewSplitModules = *SplitModulesOrErr;
    SplitModules.insert(SplitModules.end(), NewSplitModules.begin(),
                        NewSplitModules.end());
  }

  if (Verbose) {
    auto InputFilesStr = llvm::join(InputFiles.begin(), InputFiles.end(), ",");
    std::string SplitOutputFilesStr;
    for (size_t I = 0, E = SplitModules.size(); I != E; ++I) {
      if (I > 0)
        SplitOutputFilesStr += ',';

      SplitOutputFilesStr += SplitModules[I].ModuleFilePath;
    }

    errs() << formatv("sycl-module-split: input: {0}, output: {1}\n",
                      InputFilesStr, SplitOutputFilesStr);
  }

  return SplitModules;
}

/// Add any llvm-spirv option that relies on a specific Triple in addition
/// to user supplied options.
/// NOTE: Any changes made here should be reflected in the similarly named
/// function in clang/lib/Driver/ToolChains/Clang.cpp.
static void
getTripleBasedSPIRVTransOpts(const ArgList &Args,
                             SmallVector<StringRef, 8> &TranslatorArgs,
                             const llvm::Triple Triple) {
  bool IsCPU = Triple.isSPIR() &&
               Triple.getSubArch() == llvm::Triple::SPIRSubArch_x86_64;
  TranslatorArgs.push_back("-spirv-debug-info-version=nonsemantic-shader-200");
  std::string UnknownIntrinsics("-spirv-allow-unknown-intrinsics=llvm.genx.");
  if (IsCPU)
    UnknownIntrinsics += ",llvm.fpbuiltin";
  TranslatorArgs.push_back(Args.MakeArgString(UnknownIntrinsics));

  // Disable all the extensions by default
  std::string ExtArg("-spirv-ext=-all");
  std::string DefaultExtArg =
      ",+SPV_EXT_shader_atomic_float_add,+SPV_EXT_shader_atomic_float_min_max"
      ",+SPV_KHR_no_integer_wrap_decoration,+SPV_KHR_float_controls"
      ",+SPV_KHR_expect_assume,+SPV_KHR_linkonce_odr";
  std::string INTELExtArg =
      ",+SPV_INTEL_subgroups,+SPV_INTEL_media_block_io"
      ",+SPV_INTEL_device_side_avc_motion_estimation"
      ",+SPV_INTEL_fpga_loop_controls,+SPV_INTEL_unstructured_loop_controls"
      ",+SPV_INTEL_fpga_reg,+SPV_INTEL_blocking_pipes"
      ",+SPV_INTEL_function_pointers,+SPV_INTEL_kernel_attributes"
      ",+SPV_INTEL_io_pipes,+SPV_INTEL_inline_assembly"
      ",+SPV_INTEL_arbitrary_precision_integers"
      ",+SPV_INTEL_float_controls2,+SPV_INTEL_vector_compute"
      ",+SPV_INTEL_fast_composite"
      ",+SPV_INTEL_arbitrary_precision_fixed_point"
      ",+SPV_INTEL_arbitrary_precision_floating_point"
      ",+SPV_INTEL_variable_length_array,+SPV_INTEL_fp_fast_math_mode"
      ",+SPV_INTEL_long_composites"
      ",+SPV_INTEL_arithmetic_fence"
      ",+SPV_INTEL_global_variable_decorations"
      ",+SPV_INTEL_cache_controls"
      ",+SPV_INTEL_fpga_buffer_location"
      ",+SPV_INTEL_fpga_argument_interfaces"
      ",+SPV_INTEL_fpga_invocation_pipelining_attributes"
      ",+SPV_INTEL_fpga_latency_control"
      ",+SPV_KHR_shader_clock"
      ",+SPV_INTEL_bindless_images"
      ",+SPV_INTEL_task_sequence";
  ExtArg = ExtArg + DefaultExtArg + INTELExtArg;
  ExtArg += ",+SPV_INTEL_bfloat16_conversion"
            ",+SPV_INTEL_joint_matrix"
            ",+SPV_INTEL_hw_thread_queries"
            ",+SPV_KHR_uniform_group_instructions"
            ",+SPV_INTEL_masked_gather_scatter"
            ",+SPV_INTEL_tensor_float32_conversion"
            ",+SPV_INTEL_optnone"
            ",+SPV_KHR_non_semantic_info"
            ",+SPV_KHR_cooperative_matrix"
            ",+SPV_EXT_shader_atomic_float16_add"
            ",+SPV_INTEL_fp_max_error";
  TranslatorArgs.push_back(Args.MakeArgString(ExtArg));
}

/// Run LLVM to SPIR-V translation.
/// Converts 'File' from LLVM bitcode to SPIR-V format using llvm-spirv tool.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// llvm-spirv tool.
static Expected<StringRef> runLLVMToSPIRVTranslation(StringRef File,
                                                     const ArgList &Args) {
  Expected<std::string> LLVMToSPIRVPath =
      findProgram(Args, "llvm-spirv", {getMainExecutable("llvm-spirv")});
  if (!LLVMToSPIRVPath)
    return LLVMToSPIRVPath.takeError();

  SmallVector<StringRef, 8> CmdArgs;
  CmdArgs.push_back(*LLVMToSPIRVPath);
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  getTripleBasedSPIRVTransOpts(Args, CmdArgs, Triple);
  StringRef LLVMToSPIRVOptions;
  if (Arg *A = Args.getLastArg(OPT_llvm_spirv_options_EQ))
    LLVMToSPIRVOptions = A->getValue();
  LLVMToSPIRVOptions.split(CmdArgs, " ", /* MaxSplit = */ -1,
                           /* KeepEmpty = */ false);
  CmdArgs.push_back("-o");

  // Create a new file to write the translated file to.
  auto TempFileOrErr = createOutputFile(sys::path::filename(OutputFile), "spv");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();

  CmdArgs.push_back(*TempFileOrErr);
  CmdArgs.push_back(File);
  if (Error Err = executeCommands(*LLVMToSPIRVPath, CmdArgs))
    return std::move(Err);

  return *TempFileOrErr;
}

/// Adds all AOT backend options required for SYCL AOT compilation step to
/// 'CmdArgs'.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate backend options required to be passed
/// into the SYCL AOT compilation step.
/// IsCPU is a bool used to direct option generation. If IsCPU is false, then
/// options are generated for AOT compilation targeting Intel GPUs.
static void addBackendOptions(const ArgList &Args,
                              SmallVector<StringRef, 8> &CmdArgs, bool IsCPU) {
  StringRef OptC =
      Args.getLastArgValue(OPT_sycl_backend_compile_options_from_image_EQ);
  OptC.split(CmdArgs, " ", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  StringRef OptL =
      Args.getLastArgValue(OPT_sycl_backend_link_options_from_image_EQ);
  OptL.split(CmdArgs, " ", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  StringRef OptTool = (IsCPU) ? Args.getLastArgValue(OPT_cpu_tool_arg_EQ)
                              : Args.getLastArgValue(OPT_gpu_tool_arg_EQ);
  OptTool.split(CmdArgs, " ", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  return;
}

/// Run AOT compilation for Intel CPU.
/// Calls opencl-aot tool to generate device code for Intel CPU backend.
/// 'InputFile' is the input SPIR-V file.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// SYCL AOT compilation step.
static Expected<StringRef> runAOTCompileIntelCPU(StringRef InputFile,
                                                 const ArgList &Args) {
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  SmallVector<StringRef, 8> CmdArgs;
  Expected<std::string> OpenCLAOTPath =
      findProgram(Args, "opencl-aot", {getMainExecutable("opencl-aot")});
  if (!OpenCLAOTPath)
    return OpenCLAOTPath.takeError();

  CmdArgs.push_back(*OpenCLAOTPath);
  CmdArgs.push_back("--device=cpu");
  addBackendOptions(Args, CmdArgs, /* IsCPU */ true);
  // Create a new file to write the translated file to.
  auto TempFileOrErr = createOutputFile(sys::path::filename(OutputFile), "out");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();
  CmdArgs.push_back("-o");
  CmdArgs.push_back(*TempFileOrErr);
  CmdArgs.push_back(InputFile);
  if (Error Err = executeCommands(*OpenCLAOTPath, CmdArgs))
    return std::move(Err);
  return *TempFileOrErr;
}

/// Run AOT compilation for Intel GPU
/// Calls ocloc tool to generate device code for Intel GPU backend.
/// 'InputFile' is the input SPIR-V file.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// SYCL AOT compilation step.
static Expected<StringRef> runAOTCompileIntelGPU(StringRef InputFile,
                                                 const ArgList &Args) {
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  StringRef Arch(Args.getLastArgValue(OPT_arch_EQ));
  SmallVector<StringRef, 8> CmdArgs;
  Expected<std::string> OclocPath =
      findProgram(Args, "ocloc", {getMainExecutable("ocloc")});
  if (!OclocPath)
    return OclocPath.takeError();

  CmdArgs.push_back(*OclocPath);
  // The next line prevents ocloc from modifying the image name
  CmdArgs.push_back("-output_no_suffix");
  CmdArgs.push_back("-spirv_input");
  if (!Arch.empty()) {
    CmdArgs.push_back("-device");
    CmdArgs.push_back(Arch);
  }
  addBackendOptions(Args, CmdArgs, /* IsCPU */ false);
  // Create a new file to write the translated file to.
  auto TempFileOrErr = createOutputFile(sys::path::filename(OutputFile), "out");
  if (!TempFileOrErr)
    return TempFileOrErr.takeError();
  CmdArgs.push_back("-output");
  CmdArgs.push_back(*TempFileOrErr);
  CmdArgs.push_back("-file");
  CmdArgs.push_back(InputFile);
  if (Error Err = executeCommands(*OclocPath, CmdArgs))
    return std::move(Err);
  return *TempFileOrErr;
}

/// Run AOT compilation for Intel CPU/GPU.
/// 'InputFile' is the input SPIR-V file.
/// 'Args' encompasses all arguments required for linking and wrapping device
/// code and will be parsed to generate options required to be passed into the
/// SYCL AOT compilation step.
static Expected<StringRef> runAOTCompile(StringRef InputFile,
                                         const ArgList &Args) {
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  if (Triple.isSPIRAOT()) {
    if (Triple.getSubArch() == llvm::Triple::SPIRSubArch_gen)
      return runAOTCompileIntelGPU(InputFile, Args);
    if (Triple.getSubArch() == llvm::Triple::SPIRSubArch_x86_64)
      return runAOTCompileIntelCPU(InputFile, Args);
  }
  return createStringError(inconvertibleErrorCode(),
                           "Unsupported SYCL Triple and Arch");
}

Expected<StringRef> linkDevice(ArrayRef<StringRef> InputFiles,
                               const ArgList &Args) {
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  switch (Triple.getArch()) {
  case Triple::nvptx:
  case Triple::nvptx64:
  case Triple::amdgcn:
  case Triple::x86:
  case Triple::x86_64:
  case Triple::aarch64:
  case Triple::aarch64_be:
  case Triple::ppc64:
  case Triple::ppc64le:
  case Triple::systemz:
    return generic::clang(InputFiles, Args);
  case Triple::spirv32:
  case Triple::spirv64:
  case Triple::spir:
  case Triple::spir64: {
    if (Triple.getSubArch() != llvm::Triple::NoSubArch &&
        Triple.getSubArch() != llvm::Triple::SPIRSubArch_gen &&
        Triple.getSubArch() != llvm::Triple::SPIRSubArch_x86_64)
      return createStringError(
          inconvertibleErrorCode(),
          "For SPIR targets, Linking is supported only for JIT compilations "
          "and AOT compilations for Intel CPUs/GPUs");
    auto SPVFile = runLLVMToSPIRVTranslation(InputFiles[0], Args);
    if (!SPVFile)
      return SPVFile.takeError();
    bool NeedAOTCompile =
        (Triple.getSubArch() == llvm::Triple::SPIRSubArch_gen ||
          Triple.getSubArch() == llvm::Triple::SPIRSubArch_x86_64);
    auto AOTFile =
        (NeedAOTCompile) ? runAOTCompile(*SPVFile, Args) : *SPVFile;
    if (!AOTFile)
      return AOTFile.takeError();
    return NeedAOTCompile ? *AOTFile : *SPVFile;
  }
  case Triple::loongarch64:
    return generic::clang(InputFiles, Args);
  default:
    return createStringError(Triple.getArchName() +
                             " linking is not supported");
  }
}

Error runSYCLLink(ArrayRef<StringRef> Files, const ArgList &Args) {
  llvm::TimeTraceScope TimeScope("SYCLDeviceLink");
  {
    // Link the input device files using the device linker for SYCL
    // offload.
    auto TmpOutputOrErr = linkDeviceBitcode(Files, Args);
    if (!TmpOutputOrErr)
      return TmpOutputOrErr.takeError();
    SmallVector<StringRef> InputFilesSYCL;
    InputFilesSYCL.emplace_back(*TmpOutputOrErr);
    auto SplitModulesOrErr =
        UseSYCLPostLinkTool
            ? runSYCLPostLinkTool(InputFilesSYCL, Args)
            : runSYCLSplitLibrary(InputFilesSYCL, Args, *SYCLModuleSplitMode);
    if (!SplitModulesOrErr)
      return SplitModulesOrErr.takeError();

    auto &SplitModules = *SplitModulesOrErr;
    const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
    if ((Triple.isNVPTX() || Triple.isAMDGCN()) &&
        Args.hasArg(OPT_sycl_embed_ir)) {
      // When compiling for Nvidia/AMD devices and the user requested the
      // IR to be embedded in the application (via option), run the output
      // of sycl-post-link (filetable referencing LLVM Bitcode + symbols)
      // through the offload wrapper and link the resulting object to the
      // application.
      // ARV: Write SplitModules to file and exit
      return Error::success();
    }
    for (size_t I = 0, E = SplitModules.size(); I != E; ++I) {
      SmallVector<StringRef> Files = {SplitModules[I].ModuleFilePath};
      StringRef Arch = Args.getLastArgValue(OPT_arch_EQ);
      if (Arch.empty())
        Arch = "native";
      SmallVector<std::pair<StringRef, StringRef>, 4> BundlerInputFiles;
      auto ClangOutputOrErr = linkDevice(Files, Args);
      if (!ClangOutputOrErr)
        return ClangOutputOrErr.takeError();
      if (Triple.isNVPTX()) {
        auto VirtualArch = StringRef(clang::OffloadArchToVirtualArchString(
            clang::StringToOffloadArch(Arch)));
        auto PtxasOutputOrErr = nvptx::ptxas(*ClangOutputOrErr, Args, Arch);
        if (!PtxasOutputOrErr)
          return PtxasOutputOrErr.takeError();
        BundlerInputFiles.emplace_back(*ClangOutputOrErr, VirtualArch);
        BundlerInputFiles.emplace_back(*PtxasOutputOrErr, Arch);
        auto BundledFileOrErr = nvptx::fatbinary(BundlerInputFiles, Args);
        if (!BundledFileOrErr)
          return BundledFileOrErr.takeError();
        SplitModules[I].ModuleFilePath = *BundledFileOrErr;
      } else if (Triple.isAMDGCN()) {
        BundlerInputFiles.emplace_back(*ClangOutputOrErr, Arch);
        auto BundledFileOrErr = amdgcn::fatbinary(BundlerInputFiles, Args);
        if (!BundledFileOrErr)
          return BundledFileOrErr.takeError();
        SplitModules[I].ModuleFilePath = *BundledFileOrErr;
      } else {
        SplitModules[I].ModuleFilePath = *ClangOutputOrErr;
      }
    }

    // ARV: Write SplitModules to file and exit
  }


  return Error::success();
}

} // namespace

Expected<SmallVector<StringRef>> getInput(const ArgList &Args) {
  // Collect all input bitcode files to be passed to the device linking stage.
  SmallVector<StringRef> BitcodeFiles;
  for (const opt::Arg *Arg : Args.filtered(OPT_INPUT)) {
    std::optional<StringRef> Filename = Arg->getValue();
    if (!Filename || !sys::fs::exists(*Filename) ||
        sys::fs::is_directory(*Filename))
      continue;
    file_magic Magic;
    if (auto EC = identify_magic(*Filename, Magic))
      return createStringError("Failed to open file " + *Filename);
    // TODO: Current use case involves LLVM IR bitcode files as input.
    // This will be extended to support SPIR-V IR files.
    if (Magic != file_magic::bitcode)
      return createStringError("Unsupported file type");
    BitcodeFiles.push_back(*Filename);
  }
  return BitcodeFiles;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  Executable = argv[0];
  sys::PrintStackTraceOnErrorSignal(argv[0]);

  const OptTable &Tbl = getOptTable();
  BumpPtrAllocator Alloc;
  StringSaver Saver(Alloc);
  auto Args = Tbl.parseArgs(argc, argv, OPT_INVALID, Saver, [&](StringRef Err) {
    reportError(createStringError(inconvertibleErrorCode(), Err));
  });

  if (Args.hasArg(OPT_help) || Args.hasArg(OPT_help_hidden)) {
    Tbl.printHelp(
        outs(), "clang-sycl-linker [options] <options to sycl link steps>",
        "A utility that wraps around several steps required to link SYCL "
        "device files.\n"
        "This enables LLVM IR linking, post-linking and code generation for "
        "SYCL targets.",
        Args.hasArg(OPT_help_hidden), Args.hasArg(OPT_help_hidden));
    return EXIT_SUCCESS;
  }

  if (Args.hasArg(OPT_version))
    printVersion(outs());

  Verbose = Args.hasArg(OPT_verbose);
  DryRun = Args.hasArg(OPT_dry_run);
  SaveTemps = Args.hasArg(OPT_save_temps);

  OutputFile = "a.out";
  if (Args.hasArg(OPT_o))
    OutputFile = Args.getLastArgValue(OPT_o);

  UseSYCLPostLinkTool = Args.hasFlag(OPT_use_sycl_post_link_tool,
                                     OPT_no_use_sycl_post_link_tool, true);
  if (!UseSYCLPostLinkTool && Args.hasArg(OPT_use_sycl_post_link_tool))
    reportError(createStringError("-use-sycl-post-link-tool and "
                                  "-no-use-sycl-post-link-tool options can't "
                                  "be used together."));

  if (Args.hasArg(OPT_sycl_module_split_mode_EQ)) {
    if (UseSYCLPostLinkTool)
      reportError(createStringError(
          "-sycl-module-split-mode should be used with "
          "the -no-use-sycl-post-link-tool command line option."));

    StringRef StrMode = Args.getLastArgValue(OPT_sycl_module_split_mode_EQ);
    SYCLModuleSplitMode = module_split::convertStringToSplitMode(StrMode);
    if (!SYCLModuleSplitMode)
      reportError(createStringError(
          inconvertibleErrorCode(),
          formatv("sycl-module-split-mode value isn't recognized: {0}",
                  StrMode)));
  }

  if (Args.hasArg(OPT_sycl_dump_device_code_EQ)) {
    Arg *A = Args.getLastArg(OPT_sycl_dump_device_code_EQ);
    OffloadImageDumpDir = A->getValue();
    if (OffloadImageDumpDir.empty())
      sys::path::native(OffloadImageDumpDir = "./");
    else
      OffloadImageDumpDir.append(sys::path::get_separator());
  }

  // Get the input files to pass to the linking stage.
  auto FilesOrErr = getInput(Args);
  if (!FilesOrErr)
    reportError(FilesOrErr.takeError());

  // Run SYCL linking process on the generated inputs.
  if (Error Err = runSYCLLink(*FilesOrErr, Args))
    reportError(std::move(Err));

  // Remove the temporary files created.
  if (!Args.hasArg(OPT_save_temps))
    for (const auto &TempFile : TempFiles)
      if (std::error_code EC = sys::fs::remove(TempFile))
        reportError(createFileError(TempFile, EC));

  return EXIT_SUCCESS;
}
