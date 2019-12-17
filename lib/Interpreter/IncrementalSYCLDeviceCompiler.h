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

void getSYCLCompileOpt(Interpreter *interp, std::vector<const char *> &m_Args,
                       const char *llvmdir);

///\brief The struct is responsible for storing code that a user inputs at one
/// time.
struct DumpCodeEntry {
  ///\brief 1 if code is entered through Interpreter::EvaluateInternal, and 0 if
  /// code is entered through Interpreter::DeclareInternal.
  unsigned int isStatement;
  ///\brief Content of the code the user inputs at a time.
  std::string code;
  ///\brief Transaction corresponding to the code.
  Transaction *CurT;
  ///\brief a unique number assigned by IncrementalSYCLDeviceCompiler.
  size_t m_unique;
  ///\brief (if isStatement = 1) True if the declaration is successful. Used
  /// because some declarations may have NULL transaction.
  bool declSuccess;

  DumpCodeEntry(unsigned int isStatement, const std::string &input,
                Transaction *T, bool declSuccuss = false);
};

///\brief Base class for IncrementalSYCLDeviceCompiler. Used only when lauching
/// cling without -fsycl.
class IncrementalSYCLDeviceCompilerBase {
public:
  IncrementalSYCLDeviceCompilerBase() {}
  virtual ~IncrementalSYCLDeviceCompilerBase() {}
  virtual void setExtractDeclFlag(const bool flag) {}
  virtual void setClearFlag(const bool flag) {}
  virtual bool compile(const std::string &input, Transaction *T,
                       unsigned int isStatement, bool declSuccess = false) {
    return true;
  }
  virtual void setTransaction(Transaction *T) {}
  virtual void setDeclSuccess(Transaction *T) {}
  virtual void removeCodeByTransaction(Transaction *T) {}
  virtual void addCompileArg(const std::string &arg1,
                             const std::string &arg2 = "") {}
};

///\brief The class is responsible for dumping cpp code into a cpp file and
/// compiling the cpp file using SYCL Compiler to generate .spv and kernel
/// header.
class IncrementalSYCLDeviceCompiler : public IncrementalSYCLDeviceCompilerBase {
public:
  ///\brief Map the unique number of a DumpCodeEntry to its iterator of code
  /// EntryList.
  typedef std::unordered_map<size_t, std::list<DumpCodeEntry>::iterator>
      MapUnique;
  ///\brief Used for assigning each DumpCodeEntry a unique number
  static size_t m_UniqueCounter;
  ///\brief Filename of the target cpp file.
  static const std::string dumpFile;
  ///\brief Filename of the generated kernel info header file.
  static const std::string kernelInfoFile;
  ///\brief Filename of the generated spv file.
  static const std::string spvFile;

private:
  Interpreter *m_Interpreter;
  ///\brief a list to store every DumpCodeEntry.
  std::list<DumpCodeEntry> EntryList;
  ///\brief a hash table that maps the unique number of a DumpCodeEntry to its
  /// iterator of code EntryList.
  MapUnique UniqueToEntry;
  ///\brief Store all unique numbers for the current input and make
  /// setTransaction() more easily. The vector will be cleared when user inputs
  /// new code.
  std::vector<size_t> m_Uniques;
  ///\brief Unique number for the last DumpCodeEntry.
  int lastUnique = -1;
  ///\brief True if the code is entered by the user. Used to avoid non-user code
  /// to enter the code EntryList.
  bool ExtractDeclFlag = false;
  ///\brief
  std::unique_ptr<InputValidator> m_InputValidator;
  ///\brief Transaction correspondending to the kernel info header.
  Transaction **HeadTransaction = NULL;
  ///\brief If true, removeCodeByTransaction() and setDeclSuccess() are not
  /// allowed to enter and return immediately. Used for declaring kernel info
  /// header inside compileImpl().
  bool secureCode;
  ///\brief Arguments to contruct CompilerInstance in refactorCode().
  std::vector<const char *> m_Args;
  ///\brief Record .I arguments that the user inputs through cling.
  std::vector<std::string> m_ICommandInclude;
  ///\brief Path of the SYCL compiler executable
  const std::string SYCL_BIN_PATH;

public:
  IncrementalSYCLDeviceCompiler(Interpreter *interp, std::string SYCL_BIN_PATH,
                                const char *llvmdir);
  ~IncrementalSYCLDeviceCompiler();
  ///\brief Set ExtractDeclFlag.
  ///
  ///\param [in] flag.
  void setExtractDeclFlag(const bool flag) { ExtractDeclFlag = flag; }

  ///\brief Top level compile() called by Interpreter. Will call refactorCode()
  /// to extract delarations and compileImpl() to generate header and .spv.
  ///
  ///\param [in] input - The code the user currently inputs.
  ///\param [in] T - Transaction correspondending to the code.
  ///\param [in] isStatement - 1 if code is entered through
  /// Interpreter::EvaluateInternal, and 0 if
  /// code is entered through Interpreter::DeclareInternal.
  ///\param [in] declSuccess - true if declaration is successful
  ///
  ///\returns True if compiling is successful
  bool compile(const std::string &input, Transaction *T,
               unsigned int isStatement, bool declSuccess = false);

  ///\brief set Transaction for all DumpCodeEntry recoreded in m_Uniques.
  ///
  ///\param [in] T - Transaction correspondending to the code.
  void setTransaction(Transaction *T);

  ///\brief set declSuccess flag to true for all DumpCodeEntry recoreded in
  /// m_Uniques.
  ///
  ///\param [in] T - Transaction correspondending to the code.
  void setDeclSuccess(Transaction *T);

  ///\brief Remove DumpCodeEntry from EntryList identified by a Transaction.
  ///
  ///\param [in] T - Transaction correspondending to the code that will be
  ///removed.
  void removeCodeByTransaction(Transaction *T);

  ///\brief Add compiler arguments. Called by Interpreter::AddIncludePaths().
  ///
  ///\param [in] arg1 - Argument 1.
  ///\param [in] arg2 - Argument 2.
  void addCompileArg(const std::string &arg1, const std::string &arg2 = "");

  ///\brief Wrap the input code by a unique function. For instance, void
  ///__cling_custom_sycl_13.
  ///
  ///\param [in] Input - The content of the input code.
  ///\param [in] is_statement - If false, do nothing and return immediately.
  ///
  ///\returns The wrapped code.
  static std::string SyclWrapInput(const std::string &Input,
                                   unsigned int is_statement);

private:
  ///\brief Dump the code of every DumpCodeEntry in EntryList to a target file.
  ///
  ///\param [in] target - Filename of the target file.
  void dump(const std::string &target);

  ///\brief Call SYCL compiler to generate .spv and kernel info header. Load the
  /// header into cling.
  ///
  ///\returns True if compiling is successful.
  bool compileImpl();

  ///\brief Insert a DumpCodeEntry to EntryList
  ///
  ///\param [in] isStatement - 1 if code is entered through
  /// Interpreter::EvaluateInternal, and 0 if
  /// code is entered through Interpreter::DeclareInternal.
  ///\param [in] input - Content of the input code.
  ///\param [in] T - Transaction correspondent to the code.
  void insertCodeEntry(unsigned int is_statement, const std::string &input,
                       Transaction *T);
  ///\brief Use a CompilerInstance to build a AST and extract declaration out of
  /// wrapper function. Then modify the code of DumpCodeEntry in EntryList.
  ///
  ///\returns True if compiling is successful
  bool refactorCode();
};

} // namespace cling

#endif // CLING_INCREMENTAL_SYCL_COMPILER_H