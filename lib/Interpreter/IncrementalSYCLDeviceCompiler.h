#ifndef CLING_INCREMENTAL_SYCL_COMPILER_H
#define CLING_INCREMENTAL_SYCL_COMPILER_H

#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

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
  bool declSuccess;
  DumpCodeEntry(unsigned int isStatement, const std::string &input,
                Transaction *T, bool declSuccuss = false);
};

///\brief The class is responsible for dump cpp code into dump.cpp
/// and then to be compiled by syclcompiler
class IncrementalSYCLDeviceCompiler {
private:
  Interpreter *m_Interpreter;
  std::list<DumpCodeEntry> EntryList;
  std::ofstream DumpOut;
  bool ExtractDeclFlag = false;
  Transaction *CurT;
  std::unique_ptr<InputValidator> m_InputValidator;
  ///\brief Transaction of the SYCL kernel head file
  ///
  Transaction **HeadTransaction = 0;
  bool secureCode;
  bool ClearFlag = false;

public:
  IncrementalSYCLDeviceCompiler(Interpreter *interp);
  ~IncrementalSYCLDeviceCompiler();
  void setExtractDeclFlag(const bool flag);
  void setClearFlag(const bool flag);
  bool dump(const std::string &input, Transaction *T, unsigned int isStatement,
            size_t wrap_point, bool declSuccess = false);
  void submit();
  bool compile(const std::string &input);
  void setTransaction(Transaction *T);
  void setDeclSuccess(Transaction *T);
  void removeCodeByTransaction(Transaction *T);
};

} // namespace cling

#endif // CLING_INCREMENTAL_SYCL_COMPILER_H