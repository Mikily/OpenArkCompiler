/*
 * Copyright (c) [2019-2020] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
#include "compiler.h"
#include <cstdlib>
#include "file_utils.h"
#include "safe_exe.h"
#include "mpl_timer.h"

namespace maple {
using namespace mapleOption;

int Compiler::Exe(const MplOptions &mplOptions, const std::string &options) const {
  std::ostringstream ostrStream;
  ostrStream << GetBinPath(mplOptions) << GetBinName();
  std::string binPath = ostrStream.str();
  return SafeExe::Exe(binPath, options);
}

std::string Compiler::GetBinPath(const MplOptions &mplOptions) const {
#ifdef MAPLE_PRODUCT_EXECUTABLE  // build flag -DMAPLE_PRODUCT_EXECUTABLE
  std::string binPath = std::string(MAPLE_PRODUCT_EXECUTABLE);
  if (binPath.empty()) {
    binPath = mplOptions.GetExeFolder();
  } else {
    binPath = binPath + kFileSeperatorChar;
  }
#else
  std::string binPath = mplOptions.GetExeFolder();
#endif
  return binPath;
}

ErrorCode Compiler::Compile(const MplOptions &options, MIRModulePtr&) {
  MPLTimer timer = MPLTimer();
  LogInfo::MapleLogger() << "Starting " << GetName() << '\n';
  timer.Start();
  std::string strOption = MakeOption(options);
  if (strOption.empty()) {
    return kErrorInvalidParameter;
  }
  if (Exe(options, strOption) != 0) {
    return kErrorCompileFail;
  }
  timer.Stop();
  LogInfo::MapleLogger() << (GetName() + " consumed ") << timer.Elapsed() << "s\n";
  return kErrorNoError;
}

std::string Compiler::MakeOption(const MplOptions &options) const {
  std::map<std::string, MplOption> finalOptions;
  std::map<std::string, MplOption> defaultOptions = MakeDefaultOptions(options);
  AppendDefaultOptions(finalOptions, defaultOptions);
  AppendExtraOptions(finalOptions, options.GetExtras());
  std::ostringstream strOption;
  for (const auto &finalOption : finalOptions) {
    strOption << " " << finalOption.first << " " << finalOption.second.GetValue();
    if (options.HasSetDebugFlag()) {
      LogInfo::MapleLogger() << Compiler::GetName() << " options: " << finalOption.first << " "
                             << finalOption.second.GetValue() << '\n';
    }
  }
  strOption << " " << this->GetInputFileName(options);
  if (options.HasSetDebugFlag()) {
    LogInfo::MapleLogger() << Compiler::GetName() << " input files: " << GetInputFileName(options) << '\n';
  }
  return strOption.str();
}

void Compiler::AppendDefaultOptions(std::map<std::string, MplOption> &finalOptions,
                                    const std::map<std::string, MplOption> &defaultOptions) const {
  for (const auto &defaultIt : defaultOptions) {
    finalOptions.insert(make_pair(defaultIt.first, defaultIt.second));
  }
}

void Compiler::AppendExtraOptions(std::map<std::string, MplOption> &finalOptions,
                                  const std::map<std::string, std::vector<MplOption>> &extraOptions) const {
  const std::string &binName = GetBinName();
  auto extras = extraOptions.find(binName);
  if (extras == extraOptions.end()) {
    return;
  }
  for (const auto &secondExtras : extras->second) {
    AppendOptions(finalOptions, secondExtras.GetKey(), secondExtras.GetValue());
  }
}

std::map<std::string, MplOption> Compiler::MakeDefaultOptions(const MplOptions &options) const {
  DefaultOption rawDefaultOptions = GetDefaultOptions(options);
  std::map<std::string, MplOption> defaultOptions;
  if (rawDefaultOptions.mplOptions != nullptr) {
    for (uint32_t i = 0; i < rawDefaultOptions.length; ++i) {
      defaultOptions.insert(std::make_pair(rawDefaultOptions.mplOptions[i].GetKey(),
          rawDefaultOptions.mplOptions[i]));
    }
  }
  return defaultOptions;
}

void Compiler::AppendOptions(std::map<std::string, MplOption> &finalOptions, const std::string &key,
                             const std::string &value) const {
  auto finalOpt = finalOptions.find(key);
  if (finalOpt != finalOptions.end()) {
    finalOpt->second.SetValue(value);
  } else {
    MplOption option(key, value);
    finalOptions.insert(make_pair(key, option));
  }
}
}  // namespace maple
