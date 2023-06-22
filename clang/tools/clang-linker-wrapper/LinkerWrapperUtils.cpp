//===-- clang-linker-wrapper/LinkerWrapperUtils.cpp -utility functions ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// clang-linker-wrapper tool works as a wrapper over a linking job. This file
// contains helper and utility functions for clang-linker-wrapper tool.
//
//===---------------------------------------------------------------------===//

#include "LinkerWrapperUtils.h"

enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  OPT_##ID,
#include "LinkerWrapperOpts.inc"
  LastOption
#undef OPTION
};

// This utility function is used to gather all device library files that will be
// linked with input device codes.
// The list of files will be generated based on some options passed from driver.
Error getSYCLDeviceLibs(SmallVector<SmallString<128>, 16> &DeviceLibFiles,
                        const ArgList &Args) {
  bool NoDeviceLibs = false;
  int NumOfDeviceLibLinked = 0;
  const llvm::Triple Triple(Args.getLastArgValue(OPT_triple_EQ));
  bool isSpirvAOT = Triple.getSubArch() == llvm::Triple::SPIRSubArch_fpga ||
                    Triple.getSubArch() == llvm::Triple::SPIRSubArch_gen ||
                    Triple.getSubArch() == llvm::Triple::SPIRSubArch_x86_64;
  bool isMSVCEnv = Triple.isWindowsMSVCEnvironment();

  // Currently, all SYCL device libraries will be linked by default. Linkage
  // of "internal" libraries cannot be affected via -fno-sycl-device-lib.
  llvm::StringMap<bool> devicelib_link_info = {
      {"libc", true},          {"libm-fp32", true},   {"libm-fp64", true},
      {"libimf-fp32", true},   {"libimf-fp64", true}, {"libimf-bf16", true},
      {"libm-bfloat16", true}, {"internal", true}};
  if (Arg *A =
          Args.getLastArg(OPT_sycl_device_lib_EQ, OPT_no_sycl_device_lib_EQ)) {
    if (A->getValues().size() == 0) {
      llvm::errs()
          << "Need to emit equivalent of warn_drv_empty_joined_argument\n";
      createStringError(
          inconvertibleErrorCode(),
          "Need to emit equivalent of warn_drv_empty_joined_argument.");
    } else {
      if (A->getOption().matches(OPT_no_sycl_device_lib_EQ))
        NoDeviceLibs = true;

      for (StringRef Val : A->getValues()) {
        if (Val == "all") {
          for (const auto &K : devicelib_link_info.keys())
            devicelib_link_info[K] =
                true && (!NoDeviceLibs || K.equals("internal"));
          break;
        }
        auto LinkInfoIter = devicelib_link_info.find(Val);
        if (LinkInfoIter == devicelib_link_info.end() ||
            Val.equals("internal")) {
          llvm::errs() << "Need to emit equivalent of "
                          "err_drv_unsupported_option_argument\n";
          createStringError(inconvertibleErrorCode(),
                            "Need to emit equivalent of "
                            "err_drv_unsupported_option_argument.");
        }
        devicelib_link_info[Val] = true && !NoDeviceLibs;
      }
    }
  }

  SmallVector<SmallString<128>, 4> LibLocCandidates;
  if (Arg *A = Args.getLastArg(OPT_sycl_device_library_path_EQ)) {
    if (A->getValues().size() == 0) {
      llvm::errs()
          << "Need to emit equivalent of warn_drv_empty_joined_argument\n";
      createStringError(
          inconvertibleErrorCode(),
          "Need to emit equivalent of warn_drv_empty_joined_argument.");
    } else {
      for (StringRef Val : A->getValues()) {
        LibLocCandidates.push_back(Val);
      }
    }
    llvm::errs() << "LibLocCandidates:\n";
    for (const auto &LLCandidate : LibLocCandidates) {
      llvm::errs() << LLCandidate << "\n";
    }
  }
  StringRef LibSuffix = isMSVCEnv ? ".obj" : ".o";
  using SYCLDeviceLibsList = SmallVector<DeviceLibOptInfo, 5>;

  const SYCLDeviceLibsList sycl_device_wrapper_libs = {
    {"libsycl-crt", "libc"},
    {"libsycl-complex", "libm-fp32"},
    {"libsycl-complex-fp64", "libm-fp64"},
    {"libsycl-cmath", "libm-fp32"},
    {"libsycl-cmath-fp64", "libm-fp64"},
#if defined(_WIN32)
    {"libsycl-msvc-math", "libm-fp32"},
#endif
    {"libsycl-imf", "libimf-fp32"},
    {"libsycl-imf-fp64", "libimf-fp64"},
    {"libsycl-imf-bf16", "libimf-bf16"},
  };
  // For AOT compilation, we need to link sycl_device_fallback_libs as
  // default too.
  const SYCLDeviceLibsList sycl_device_fallback_libs = {
      {"libsycl-fallback-cassert", "libc"},
      {"libsycl-fallback-cstring", "libc"},
      {"libsycl-fallback-complex", "libm-fp32"},
      {"libsycl-fallback-complex-fp64", "libm-fp64"},
      {"libsycl-fallback-cmath", "libm-fp32"},
      {"libsycl-fallback-cmath-fp64", "libm-fp64"},
      {"libsycl-fallback-imf", "libimf-fp32"},
      {"libsycl-fallback-imf-fp64", "libimf-fp64"},
      {"libsycl-fallback-imf-bf16", "libimf-bf16"}};
  const SYCLDeviceLibsList sycl_device_bfloat16_fallback_lib = {
      {"libsycl-fallback-bfloat16", "libm-bfloat16"}};
  const SYCLDeviceLibsList sycl_device_bfloat16_native_lib = {
      {"libsycl-native-bfloat16", "libm-bfloat16"}};
  // ITT annotation libraries are linked in separately whenever the device
  // code instrumentation is enabled.
  const SYCLDeviceLibsList sycl_device_annotation_libs = {
      {"libsycl-itt-user-wrappers", "internal"},
      {"libsycl-itt-compiler-wrappers", "internal"},
      {"libsycl-itt-stubs", "internal"}};
  auto addInputs = [&](const SYCLDeviceLibsList &LibsList) {
    bool LibLocSelected = false;
#if 1
    for (const auto &LLCandidate : LibLocCandidates) {
      if (LibLocSelected)
        break;
      for (const DeviceLibOptInfo &Lib : LibsList) {
        if (!devicelib_link_info[Lib.devicelib_option])
          continue;
        SmallString<128> LibName(LLCandidate);
        llvm::sys::path::append(LibName, Lib.devicelib_name);
        llvm::sys::path::replace_extension(LibName, LibSuffix);
        if (llvm::sys::fs::exists(LibName)) {
          ++NumOfDeviceLibLinked;
          DeviceLibFiles.push_back(LibName);
          if (!LibLocSelected)
            LibLocSelected = !LibLocSelected;
        }
      }
    }
#endif
  };

  addInputs(sycl_device_wrapper_libs);
  if (isSpirvAOT || Triple.isNVPTX())
    addInputs(sycl_device_fallback_libs);

  if (Arg *A = Args.getLastArg(OPT_sycl_bfloat_lib_EQ)) {
    // Add native or fallback bfloat16 library.
    if (A->getValue() == "native")
      addInputs(sycl_device_bfloat16_native_lib);
    else if (A->getValue() == "fallback")
      addInputs(sycl_device_bfloat16_fallback_lib);
    else {
      llvm::errs() << "Need to emit error for invalid entry\n";
      createStringError(inconvertibleErrorCode(),
                        "Need to emit error for invalid entry.");
    }
  }

  if (Args.getLastArg(OPT_sycl_instrument_device_code))
    addInputs(sycl_device_annotation_libs);
  return Error::success();
}