//===- llvmtuner/src/LoopFuncNaming.h: global loop, func naming -*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LOOP_FUNC_NAME_H
#define LOOP_FUNC_NAME_H

#include <string>
#include <cstdlib>
#include <cstdio>
#include <assert.h>

class LoopName {
  std::string resolvedModuleName;
  std::string functionName;
  unsigned loopId;

  // Writing out and reading back the loop name to a stream
  std::ostream& operator <<(std::ostream& os);
  std::istream& operator >>(std::istream& is);

public:
  LoopName(std::string moduleName, std::string funcName, unsigned _loopId) :
  resolvedModuleName(realpath(moduleName.c_str(), NULL)),
    functionName(funcName),
    loopId(_loopId)
  {}
  
  // Construct a LoopName from a formatted string, Arg, with format:
  // "function-name , integer-loop-id"
  // moduleName must be part of the func name (i.e, func must be qualified).
  LoopName(const std::string& Arg);

  // Accessor methods to retrieve components of the name
  const std::string& getModule()   const { return resolvedModuleName; }
  const std::string& getFuncName() const { return functionName; }
  int                getLoopId()   const { return loopId; }
  std::string        toString()    const;
};

// Construct a LoopName from a formatted string, Arg, with format:
// "function-name , integer-loop-id"
// moduleName must be part of the func name (i.e, func must be qualified).
LoopName::LoopName(const std::string& Arg):
  resolvedModuleName("")
{
  size_t sep = Arg.find(',');
  
  // ill-formated string
  assert(sep < Arg.length() - 1 && "Ill-formatted string initializer");
  
  functionName = Arg.substr(0, sep);
  loopId = std::atoi(Arg.substr(sep+1).c_str());
  assert (loopId > 0 && "Header id must be a positive integer\n");
}

// Print the fully qualified loop ID to a new string
std::string LoopName::toString() const
{
  std::string asString;
  asString = (getModule().length() > 0)? getModule() + ":" : "";
  asString += getFuncName() + ":" + std::to_string(getLoopId());
  return asString;
}

inline std::ostream &operator<<(std::ostream &os, const LoopName& loopName)
{
  os << loopName.toString();
  return os;
}

#if 0
inline std::istream& operator>>(std::istream& is)
{
  // Read data from stream, construct a LoopName, and return it
  return is;
}
#endif


#endif	 //LOOP_FUNC_NAME_H
