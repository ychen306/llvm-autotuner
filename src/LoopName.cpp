//===- llvmtuner/src/LoopName.cpp: global loop, func naming -------*-C++-*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <string>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <iostream>
#include "LoopName.h"

// Construct a LoopName from a formatted string, Arg, with format:
// "function-name,integer-loop-id"
// moduleName must be part of the func name (i.e, func must be qualified).
LoopName::LoopName(const std::string& Arg):
  resolvedModuleName(""), loopId(-1)
{
  size_t sep = Arg.find(',');
  
  // ill-formated string
  if (sep >= Arg.length() - 1) {
    std::cerr << "Ill-formatted string initializer";
    return;
  }
  
  functionName = Arg.substr(0, sep);
  loopId = std::atoi(Arg.substr(sep+1).c_str());
  if (loopId > 0)
    std::cerr << "LoopName: Header id must be a positive integer\n";
}

LoopName::LoopName(std::string moduleName,
		   std::string funcName, unsigned _loopId) :
  resolvedModuleName(moduleName),
  functionName(funcName),
  loopId(_loopId)
{}

// Print the fully qualified loop ID to a new string
std::string LoopName::toString() const
{
  std::string asString;
  asString = (getModule().length() > 0)? getModule() + ":" : "";
  asString += getFuncName() + ":" + std::to_string(getLoopId());
  return asString;
}
