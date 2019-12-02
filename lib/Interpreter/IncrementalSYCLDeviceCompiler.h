#ifndef CLING_INCREMENTAL_SYCL_COMPILER_H
#define CLING_INCREMENTAL_SYCL_COMPILER_H

#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cling {
class Transaction;
class InputValidator;
class Interpreter;
} // namespace cling

namespace cling {

class DumpCodeEntry {
public:
  unsigned int isStatement;
  std::string code;
  Transaction *CurT;
  size_t m_unique;
  bool declSuccess;
  DumpCodeEntry(unsigned int isStatement, const std::string &input,
                Transaction *T, bool declSuccuss = false);
};

///\brief The class is responsible for dump cpp code into dump.cpp
/// and then to be compiled by syclcompiler
class IncrementalSYCLDeviceCompiler {
public:
  typedef std::unordered_map<size_t, std::list<DumpCodeEntry>::iterator>
      MapUnique;
  static size_t m_UniqueCounter;

private:
  Interpreter *m_Interpreter;
  std::list<DumpCodeEntry> EntryList;
  MapUnique UniqueToEntry;
  std::vector<size_t> m_Uniques;
  int lastUnique = -1;
  std::ofstream DumpOut;
  bool ExtractDeclFlag = false;
  Transaction *CurT;
  std::unique_ptr<InputValidator> m_InputValidator;
  ///\brief Transaction of the SYCL kernel head file
  ///
  Transaction **HeadTransaction = 0;
  bool secureCode;
  bool ClearFlag = false;
  const std::string dumpFile = "dump.cpp";
  std::vector<const char *> m_Args;
  std::vector<std::string> m_ICommandInclude;

public:
  IncrementalSYCLDeviceCompiler(Interpreter *interp);
  ~IncrementalSYCLDeviceCompiler();
  void setExtractDeclFlag(const bool flag) { ExtractDeclFlag = flag; }
  void setClearFlag(const bool flag) { ClearFlag = flag; }
  bool compile(const std::string &input, Transaction *T,
               unsigned int isStatement, size_t wrap_point,
               bool declSuccess = false);
  void setTransaction(Transaction *T);
  void setDeclSuccess(Transaction *T);
  void removeCodeByTransaction(Transaction *T);
  void addCompileArg(const std::string &arg1, const std::string &arg2 = "");
  bool refactorCode();
  static std::string SyclWrapInput(const std::string &Input,
                                   unsigned int is_statement);

private:
  void dump(const std::string &target);
  bool compileImpl(const std::string &input);
  void insertCodeEntry(unsigned int is_statement,
                       const std::string &complete_input, Transaction *T);
};

} // namespace cling

#endif // CLING_INCREMENTAL_SYCL_COMPILER_H