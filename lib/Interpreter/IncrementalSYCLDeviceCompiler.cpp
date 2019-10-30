#include "IncrementalSYCLDeviceCompiler.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/MetaProcessor/InputValidator.h"
#include "cling/Utils/SourceNormalization.h"

#include "clang/Frontend/CompilerInstance.h"

namespace cling {
DumpCodeEntry::DumpCodeEntry(unsigned int isStatement, const std::string &input,
                             Transaction *T, bool declSuccess /* = false*/)
    : isStatement(isStatement), code(input), CurT(T), declSuccess(declSuccess) {
}

IncrementalSYCLDeviceCompiler::IncrementalSYCLDeviceCompiler(
    Interpreter *interp)
    : m_Interpreter(interp) {
  m_InputValidator.reset(new InputValidator());
  HeadTransaction = new Transaction *;
  *HeadTransaction = NULL;
  secureCode = false;
  DumpOut.open("dump.cpp", std::ios::in | std::ios::out | std::ios::trunc);
  DumpOut.close();
}
IncrementalSYCLDeviceCompiler::~IncrementalSYCLDeviceCompiler() {
  delete HeadTransaction;
}

void IncrementalSYCLDeviceCompiler::setExtractDeclFlag(const bool flag) {
  ExtractDeclFlag = flag;
}

bool IncrementalSYCLDeviceCompiler::dump(const std::string &input,
                                         Transaction *T,
                                         unsigned int isStatement,
                                         bool declSuccess /* = false*/) {
  return dump(input, T, isStatement, std::string::npos, declSuccess);
}

bool IncrementalSYCLDeviceCompiler::dump(const std::string &input,
                                         Transaction *T,
                                         unsigned int isStatement,
                                         size_t wrap_point,
                                         bool declSuccess /* = false*/) {
  if ((isStatement == 0) && (!ExtractDeclFlag))
    return true;
  if (isStatement == 0) {
    EntryList.push_back(DumpCodeEntry(isStatement, input, T, declSuccess));
    submit();
    return compile(input);
  }
  std::istringstream input_holder(input);
  std::string line;
  std::string complete_input;
  while (getline(input_holder, line)) {
    if (line.empty() || (line.size() == 1 && line.front() == '\n')) {
      continue;
    }
    if (m_InputValidator->validate(line) == InputValidator::kIncomplete) {
      continue;
    }
    complete_input.clear();
    m_InputValidator->reset(&complete_input);
    size_t wrapPoint = std::string::npos;
    wrapPoint = utils::getWrapPoint(complete_input,
                                    m_Interpreter->getCI()->getLangOpts());
    if (wrapPoint == std::string::npos)
      EntryList.push_back(DumpCodeEntry(0, complete_input, T));
    else if (wrapPoint == 0)
      EntryList.push_back(DumpCodeEntry(1, complete_input, T));
    else {
      EntryList.push_back(
          DumpCodeEntry(0, complete_input.substr(0, wrapPoint), T));
      submit();
      EntryList.push_back(
          DumpCodeEntry(1, complete_input.substr(wrapPoint), T));
    }
    submit();
  }
  return compile(input);
}

void IncrementalSYCLDeviceCompiler::submit() {
  std::string declCode;
  std::string stmtCode;
  for (auto &CodeEntry : EntryList) {
    std::string input(CodeEntry.code);
    if (CodeEntry.isStatement == 0) {
      declCode = declCode + input + '\n';
    } else {
      stmtCode = stmtCode + input;
      for (auto sit = input.rbegin(); sit != input.rend(); sit++) {
        if (*sit != ' ') {
          if (*sit == ';')
            stmtCode += '\n';
          else
            stmtCode += ";\n";
          break;
        }
      }
    }
  }
  DumpOut.open("dump.cpp", std::ios::in | std::ios::out | std::ios::trunc);
  DumpOut.seekp(0, std::ios::beg);
  DumpOut << declCode << "int main(){\n" + stmtCode + "}";
  DumpOut.close();
  ExtractDeclFlag = false;
}

bool IncrementalSYCLDeviceCompiler::compile(const std::string &input) {
  std::string _hsinput(input);
  // bool head_spv_flg =
  // utils::generate_hppandspv(_hsinput,m_Interpreter->getCI()->getLangOpts());
  secureCode = true;
  if (true) {
    DumpOut.open("st.h", std::ios::in | std::ios::out | std::ios::trunc);
    DumpOut.close();
    int sysReturn =
        std::system("clang++ -Wno-unused-value --sycl -fno-sycl-use-bitcode "
                    "-Xclang -fsycl-int-header=st.h -c dump.cpp -o mk.spv");
    if (sysReturn != 0) {
      secureCode = false;
      removeCodeByTransaction(NULL);
      return false;
    }
    if (HeadTransaction && HeadTransaction[0]) {
      m_Interpreter->unload(HeadTransaction[0][0]);
    }
    std::ifstream headFile;
    headFile.open("st.h");
    std::ostringstream tmp;
    tmp << headFile.rdbuf();
    std::string headFileContent = tmp.str();
    headFile.close();
    if (m_Interpreter->declare(headFileContent.c_str(), HeadTransaction) !=
        Interpreter::kSuccess) {
      secureCode = false;
      return false;
    }
  }
  secureCode = false;
  return true;
}

void IncrementalSYCLDeviceCompiler::setTransaction(Transaction *T) {
  for (auto it = EntryList.rbegin(); it != EntryList.rend(); it++) {
    if (it->CurT)
      break;
    else
      it->CurT = T;
  }
}

void IncrementalSYCLDeviceCompiler::setDeclSuccess(Transaction *T) {
  if (!secureCode) {
    if (T) {
      setTransaction(T);
    } else {
      for (auto it = EntryList.rbegin(); it != EntryList.rend(); it++) {
        if (!it->CurT) {
          it->declSuccess = true;
        }
      }
    }
  }
}

void IncrementalSYCLDeviceCompiler::removeCodeByTransaction(Transaction *T) {
  if (!secureCode) {
    for (auto it = EntryList.begin(); it != EntryList.end();) {
      if (!it->declSuccess && (it->CurT == NULL || it->CurT == T)) {
        it = EntryList.erase(it);
      } else {
        it++;
      }
    }
  }
}
} // namespace cling