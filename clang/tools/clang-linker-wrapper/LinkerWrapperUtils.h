//===- LinkerWrapperUtils.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_CLANG_LINKER_WRAPPER_LINKER_WRAPPER_UTILS_H
#define LLVM_CLANG_TOOLS_CLANG_LINKER_WRAPPER_LINKER_WRAPPER_UTILS_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;
using namespace llvm::opt;

struct DeviceLibOptInfo {
  StringRef devicelib_name;
  StringRef devicelib_option;
};

Error getSYCLDeviceLibs(SmallVector<SmallString<128>, 16> &DeviceLibFiles,
                        const ArgList &Args);

#endif