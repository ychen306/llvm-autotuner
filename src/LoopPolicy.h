//===- llvmtuner/src/LoopPolicy.h: policies to extract loops ----*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// class LoopPolicy:
// Describes policies for multiple modules.  Each one is a ModulePolicyInfo.
//
//===----------------------------------------------------------------------===//

#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <iostream>

class ModulePolicyInfo;


//===----------------------------------------------------------------------===//

class LoopPolicy
{
  // Types used within this class
  typedef std::map<const std::string, ModulePolicyInfo*> PolicyMap;
  typedef       PolicyMap::iterator       iterator;
  typedef const PolicyMap::iterator const_iterator;
  
  // Internal state
  PolicyMap modulePolicies;

  // Look up the policy for a module.  Insert an empty one if none exists.
  ModulePolicyInfo& getOrCreatePolicy(const std::string& moduleName);

public:
  // dtor: release memory for all ModulePoicyInfo objects in the map
  ~LoopPolicy();

  // Add a new policy for one loop in one module
  void addPolicy(const std::string& moduleName,
		 const std::string& funcName, const LoopName& loopName);

  // Writing out and reading back the policies
  void print(std::ostream &os) const;
};

extern std::ostream& operator <<(std::ostream&, const LoopPolicy&);
#if 0
  friend std::istream& operator >>(std::istream&, const LoopPolicy&);
#endif
