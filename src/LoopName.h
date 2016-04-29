//===- llvmtuner/src/LoopName.h: global loop, func naming -------*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LOOP_FUNC_NAME_H
#define LOOP_FUNC_NAME_H

#include <string>
#include <cstdio>

class LoopName {
  std::string resolvedModuleName;
  std::string functionName;
  unsigned loopId;

  // Writing out and reading back the loop name to a stream
  std::ostream& operator <<(std::ostream& os);
  std::istream& operator >>(std::istream& is);

public:
  LoopName(std::string moduleName, std::string funcName, unsigned _loopId);
  
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
