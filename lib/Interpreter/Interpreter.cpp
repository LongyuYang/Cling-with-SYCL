//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Lukasz Janyst <ljanyst@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/Interpreter/Interpreter.h"
#include "cling/Utils/Paths.h"
#ifdef LLVM_ON_WIN32
#include "cling/Utils/Platform.h"
#endif
#include "ClingUtils.h"

#include "DynamicLookup.h"
#include "EnterUserCodeRAII.h"
#include "ExternalInterpreterSource.h"
#include "ForwardDeclPrinter.h"
#include "IncrementalExecutor.h"
#include "IncrementalParser.h"
#include "MultiplexInterpreterCallbacks.h"
#include "TransactionUnloader.h"
#include "IncrementalSYCLDeviceCompiler.h"

#include "cling/Interpreter/AutoloadCallback.h"
#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/ClangInternalState.h"
#include "cling/Interpreter/ClingCodeCompleteConsumer.h"
#include "cling/Interpreter/CompilationOptions.h"
#include "cling/Interpreter/DynamicExprInfo.h"
#include "cling/Interpreter/DynamicLibraryManager.h"
#include "cling/Interpreter/Exception.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Interpreter/Value.h"
#include "cling/Utils/AST.h"
#include "cling/Utils/Casting.h"
#include "cling/Utils/Output.h"
#include "cling/Utils/SourceNormalization.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/ExternalPreprocessorSource.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Path.h"

#include <sstream>
#include <string>
#include <vector>

// fixme::
bool Dflg=false;
bool MBflg=false;
bool head_spv_flg = true;
//int exec_i = -1;

using namespace clang;

namespace {

  // Forward cxa_atexit for global d'tors.
  static int local_cxa_atexit(void (*func) (void*), void* arg,
                              cling::Interpreter* Interp) {
    Interp->AddAtExitFunc(func, arg);
    return 0;
  }

  static cling::Interpreter::ExecutionResult
  ConvertExecutionResult(cling::IncrementalExecutor::ExecutionResult ExeRes) {
    switch (ExeRes) {
    case cling::IncrementalExecutor::kExeSuccess:
      return cling::Interpreter::kExeSuccess;
    case cling::IncrementalExecutor::kExeFunctionNotCompiled:
      return cling::Interpreter::kExeFunctionNotCompiled;
    case cling::IncrementalExecutor::kExeUnresolvedSymbols:
      return cling::Interpreter::kExeUnresolvedSymbols;
    default: break;
    }
    return cling::Interpreter::kExeSuccess;
  }

  static bool isPracticallyEmptyModule(const llvm::Module* M) {
    return M->empty() && M->global_empty() && M->alias_empty();
  }
} // unnamed namespace

namespace cling {

  Interpreter::PushTransactionRAII::PushTransactionRAII(const Interpreter* i)
    : m_Interpreter(i) {
    CompilationOptions CO = m_Interpreter->makeDefaultCompilationOpts();
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = 0;

    m_Transaction = m_Interpreter->m_IncrParser->beginTransaction(CO);
  }

  Interpreter::PushTransactionRAII::~PushTransactionRAII() {
    pop();
  }

  void Interpreter::PushTransactionRAII::pop() const {
    IncrementalParser::ParseResultTransaction PRT
      = m_Interpreter->m_IncrParser->endTransaction(m_Transaction);
    if (PRT.getPointer()) {
      assert(PRT.getPointer()==m_Transaction && "Ended different transaction?");
      m_Interpreter->m_IncrParser->commitTransaction(PRT);
    }
  }

  Interpreter::StateDebuggerRAII::StateDebuggerRAII(const Interpreter* i)
    : m_Interpreter(i) {
    if (m_Interpreter->isPrintingDebug()) {
      const CompilerInstance& CI = *m_Interpreter->getCI();
      CodeGenerator* CG = i->m_IncrParser->getCodeGenerator();

      // The ClangInternalState constructor can provoke deserialization,
      // we need a transaction.
      PushTransactionRAII pushedT(i);

      m_State.reset(
          new ClangInternalState(CI.getASTContext(), CI.getPreprocessor(),
                                 CG ? CG->GetModule() : nullptr, CG, "aName"));
    }
  }

  Interpreter::StateDebuggerRAII::~StateDebuggerRAII() {
    if (m_State) {
      // The ClangInternalState destructor can provoke deserialization,
      // we need a transaction.
      PushTransactionRAII pushedT(m_Interpreter);
      m_State->compare("aName", m_Interpreter->m_Opts.Verbose());
      m_State.reset();
    }
  }

  const Parser& Interpreter::getParser() const {
    return *m_IncrParser->getParser();
  }

  Parser& Interpreter::getParser() {
    return *m_IncrParser->getParser();
  }

  clang::SourceLocation Interpreter::getNextAvailableLoc() const {
    return m_IncrParser->getLastMemoryBufferEndLoc().getLocWithOffset(1);
  }

  bool Interpreter::isInSyntaxOnlyMode() const {
    return getCI()->getFrontendOpts().ProgramAction
      == clang::frontend::ParseSyntaxOnly;
  }

  bool Interpreter::isValid() const {
    // Should we also check m_IncrParser->getFirstTransaction() ?
    // not much can be done without it (its the initializing transaction)
    return m_IncrParser && m_IncrParser->isValid() &&
           m_DyLibManager && m_LookupHelper &&
           (isInSyntaxOnlyMode() || m_Executor);
  }

  namespace internal { void symbol_requester(); }

  const char* Interpreter::getVersion() {
    return ClingStringify(CLING_VERSION);
  }

  static bool handleSimpleOptions(const InvocationOptions& Opts) {
    if (Opts.ShowVersion) {
      cling::log() << Interpreter::getVersion() << '\n';
    }
    if (Opts.Help) {
      Opts.PrintHelp();
    }
    return Opts.ShowVersion || Opts.Help;
  }

  static void setupCallbacks(Interpreter& Interp,
                             const Interpreter* parentInterp) {
    // We need InterpreterCallbacks only if it is a parent Interpreter.
    if (parentInterp) return;

    // Disable suggestions for ROOT
    bool showSuggestions =
        !llvm::StringRef(ClingStringify(CLING_VERSION)).startswith("ROOT");

    std::unique_ptr<InterpreterCallbacks> AutoLoadCB(
        new AutoloadCallback(&Interp, showSuggestions));
    Interp.setCallbacks(std::move(AutoLoadCB));
  }

  // Construct a column of modulemap overlay file, given System filename,
  // Location + Filename (modulemap to be overlayed). If NotLast is true,
  // append ",".
  static std::string buildModuleMapOverlayEntry(const std::string& System,
        const std::string& Filename, const std::string& Location, bool NotLast) {
    std::string modulemap_overlay;
    modulemap_overlay += "{ 'name': '";
    modulemap_overlay += System;
    modulemap_overlay += "', 'type': 'directory',\n";
    modulemap_overlay += "'contents': [\n   { 'name': 'module.modulemap', ";
    modulemap_overlay += "'type': 'file',\n  'external-contents': '";
    modulemap_overlay += Location + "/" + Filename + "'\n";
    modulemap_overlay += "}\n ]\n }";
    if (NotLast)
      modulemap_overlay += ",\n";
    return modulemap_overlay;
  }

  Interpreter::Interpreter(int argc, const char* const *argv,
                           const char* llvmdir /*= 0*/,
                           const ModuleFileExtensions& moduleExtensions,
                           bool noRuntime, const Interpreter* parentInterp) :
    m_Opts(argc, argv),
    m_UniqueCounter(parentInterp ? parentInterp->m_UniqueCounter + 1 : 0),
    m_PrintDebug(false), m_DynamicLookupDeclared(false),
    m_DynamicLookupEnabled(false), m_RawInputEnabled(false),
    m_RedefinitionAllowed(false),
    m_OptLevel(parentInterp ? parentInterp->m_OptLevel : -1) {

    if (handleSimpleOptions(m_Opts))
      return;

    m_LLVMContext.reset(new llvm::LLVMContext);
    m_DyLibManager.reset(new DynamicLibraryManager(getOptions()));
    m_IncrParser.reset(new IncrementalParser(this, llvmdir, moduleExtensions));
    m_SYCLCompiler.reset(new IncrementalSYCLDeviceCompilerBase());
    if (!m_IncrParser->isValid(false))
      return;

    // Initialize the opt level to what CodeGenOpts says.
    if (m_OptLevel == -1)
      setDefaultOptLevel(getCI()->getCodeGenOpts().OptimizationLevel);

    Sema& SemaRef = getSema();
    Preprocessor& PP = SemaRef.getPreprocessor();
    // Enable incremental processing, which prevents the preprocessor destroying
    // the lexer on EOF token.
    PP.enableIncrementalProcessing();

    m_LookupHelper.reset(new LookupHelper(new Parser(PP, SemaRef,
                                                     /*SkipFunctionBodies*/false,
                                                     /*isTemp*/true), this));
    if (!m_LookupHelper)
      return;

    if (!isInSyntaxOnlyMode()) {
      m_Executor.reset(new IncrementalExecutor(SemaRef.Diags, *getCI()));
      if (!m_Executor)
        return;
    }

    // Tell the diagnostic client that we are entering file parsing mode.
    DiagnosticConsumer& DClient = getCI()->getDiagnosticClient();
    DClient.BeginSourceFile(getCI()->getLangOpts(), &PP);

    if (!m_Opts.OverlayFile.empty()) {
      assert(getSema().getLangOpts().Modules
             && "Using overlay without -fmodules");
      HeaderSearch& HSearch = getCI()->getPreprocessor().getHeaderSearchInfo();

      // Get system include paths
      llvm::SmallVector<std::string, 3> HSearchPaths;
      for (auto Path = HSearch.system_dir_begin();
            Path < HSearch.system_dir_end(); Path++) {
        HSearchPaths.push_back((*Path).getName());
      }

      // Virtual modulemap overlay file
      std::string MOverlay = "{\n 'version': 0,\n 'roots': [\n";

      // Check if the system path exists. If it does and it contains
      // "/include/c++/" (as stl path is always inferred from gcc path),
      // append this to MOverlay.
      // FIXME: Implement a more sophisticated way to detect stl paths
      for (auto &&SystemPath : HSearchPaths) {
        llvm::StringRef SystemPathSR = SystemPath;
        if (llvm::sys::fs::is_directory(SystemPathSR) &&
            llvm::sys::path::filename(SystemPathSR) != "backward" &&
            SystemPathSR.contains("/include/c++/")) {
          MOverlay += buildModuleMapOverlayEntry(SystemPath, "std.modulemap",
                m_Opts.OverlayFile, /*NotLast*/ true);
          break; // first one wins!
        }
      }

      // FIXME: Support system which doesn't have /usr/include as libc path.
      // We need to find out how to identify the correct libc path on such
      // system, we cannot add random include path to overlay file.
      MOverlay += buildModuleMapOverlayEntry("/usr/include", "libc.modulemap",
            m_Opts.OverlayFile, /*NotLast*/ false);

      MOverlay += "]\n }\n ]\n }\n";

      // Set up the virtual modulemap overlay file
      std::unique_ptr<llvm::MemoryBuffer> Buffer =
         llvm::MemoryBuffer::getMemBuffer(MOverlay);

      IntrusiveRefCntPtr<clang::vfs::FileSystem> FS =
         vfs::getVFSFromYAML(std::move(Buffer), nullptr, "modulemap.overlay.yaml");
      if (!FS.get())
        llvm::errs() << "Error in modulemap.overlay!\n";

      clang::CompilerInvocation &CInvo = getCI()->getInvocation();
      // Load virtual modulemap overlay file
      CInvo.addOverlay(FS);
    }

    bool usingCxxModules = getSema().getLangOpts().Modules;
    if (usingCxxModules) {
      // Explicitly create the modulemanager now. If we would create it later
      // implicitly then it would just overwrite our callbacks we set below.
      m_IncrParser->getCI()->createModuleManager();

      // When using C++ modules, we setup the callbacks now that we have them
      // ready before we parse code for the first time. Without C++ modules
      // we can't setup the calls now because the clang PCH currently just
      // overwrites it in the Initialize method and we have no simple way to
      // initialize them earlier. We handle the non-modules case below.
      setupCallbacks(*this, parentInterp);
    }

    if(m_Opts.CompilerOpts.CUDA){
       if(m_DyLibManager->loadLibrary("libcudart.so", true) ==
         cling::DynamicLibraryManager::LoadLibResult::kLoadLibNotFound){
           llvm::errs() << "Error: libcudart.so not found!\n" <<
             "Please add the cuda lib path to LD_LIBRARY_PATH or set it via -L argument.\n";
       }
    }

    llvm::SmallVector<IncrementalParser::ParseResultTransaction, 2>
      IncrParserTransactions;
    if (!m_IncrParser->Initialize(IncrParserTransactions, parentInterp)) {
      // Initialization is not going well, but we still have to commit what
      // we've been given. Don't clear the DiagnosticsConsumer so the caller
      // can inspect any errors that have been generated.
      for (auto&& I: IncrParserTransactions)
        m_IncrParser->commitTransaction(I, false);
      return;
    }

    // When not using C++ modules, we now have a PCH and we can safely setup
    // our callbacks without fearing that they get overwritten by clang code.
    // The modules setup is handled above.
    if (!usingCxxModules) {
      setupCallbacks(*this, parentInterp);
    }

    llvm::SmallVector<llvm::StringRef, 6> Syms;
    Initialize(noRuntime || m_Opts.NoRuntime, isInSyntaxOnlyMode(), Syms);

    // Commit the transactions, now that gCling is set up. It is needed for
    // static initialization in these transactions through local_cxa_atexit().
    for (auto&& I: IncrParserTransactions)
      m_IncrParser->commitTransaction(I);

    // Now that the transactions have been commited, force symbol emission
    // and overrides.
    if (!isInSyntaxOnlyMode()) {
      if (const Transaction* T = getLastTransaction()) {
        if (auto M = T->getModule()) {
          for (const llvm::StringRef& Sym : Syms) {
            const llvm::GlobalValue* GV = M->getNamedValue(Sym);
  #if defined(__linux__)
            // libstdc++ mangles at_quick_exit on Linux when g++ < 5
            if (!GV && Sym.equals("at_quick_exit"))
              GV = M->getNamedValue("_Z13at_quick_exitPFvvE");
  #endif
            if (GV) {
              if (void* Addr = m_Executor->getPointerToGlobalFromJIT(*GV))
                m_Executor->addSymbol(Sym.str().c_str(), Addr, true);
              else
                cling::errs() << Sym << " not defined\n";
            } else
              cling::errs() << Sym << " not in Module!\n";
          }
        }
      }
    }

    m_IncrParser->SetTransformers(parentInterp);

    if (!m_LLVMContext) {
      // Never true, but don't tell the compiler.
      // Force symbols needed by runtime to be included in binaries.
      // Prevents stripping the symbol due to dead-code optimization.
      internal::symbol_requester();
    }

    if (m_Opts.CompilerOpts.SYCL){   
      if (loadLibrary("libsycl.so") != Interpreter::kSuccess) {
        llvm::errs() << "enable SYCL failed: could not load libsycl.so\n";
      }
      else {
        char* SYCL_BIN_PATH_CString = getenv("SYCL_BIN_PATH");
        if (!SYCL_BIN_PATH_CString) {
          llvm::errs() << "enable SYCL failed: SYCL_BIN_PATH not set\n";
        }
        else {
          std::string SYCL_BIN_PATH(SYCL_BIN_PATH_CString);
          setenv("SYCL_USE_KERNEL_SPV", "DeviceCode.spv", 1);
          m_SYCLCompiler.reset(new IncrementalSYCLDeviceCompiler(this, SYCL_BIN_PATH, llvmdir));
        }
      }
    }
  }

  ///\brief Constructor for the child Interpreter.
  /// Passing the parent Interpreter as an argument.
  ///
  Interpreter::Interpreter(const Interpreter &parentInterpreter, int argc,
                           const char* const *argv,
                           const char* llvmdir /*= 0*/,
                           const ModuleFileExtensions& moduleExtensions/*={}*/,
                           bool noRuntime /*= true*/) :
    Interpreter(argc, argv, llvmdir, moduleExtensions, noRuntime,
                &parentInterpreter) {
    // Do the "setup" of the connection between this interpreter and
    // its parent interpreter.
    if (CompilerInstance* CI = getCIOrNull()) {
      // The "bridge" between the interpreters.
      ExternalInterpreterSource *myExternalSource =
        new ExternalInterpreterSource(&parentInterpreter, this);

      llvm::IntrusiveRefCntPtr <ExternalASTSource>
        astContextExternalSource(myExternalSource);

      CI->getASTContext().setExternalSource(astContextExternalSource);

      // Inform the Translation Unit Decl of I2 that it has to search somewhere
      // else to find the declarations.
      CI->getASTContext().getTranslationUnitDecl()->setHasExternalVisibleStorage(true);

      // Give my IncrementalExecutor a pointer to the Incremental executor of the
      // parent Interpreter.
      m_Executor->setExternalIncrementalExecutor(parentInterpreter.m_Executor.get());
    }
  }

  Interpreter::~Interpreter() {
    // Do this first so m_StoredStates will be ignored if Interpreter::unload
    // is called later on.
    for (size_t i = 0, e = m_StoredStates.size(); i != e; ++i)
      delete m_StoredStates[i];
    m_StoredStates.clear();

    if (m_Executor)
      m_Executor->shuttingDown();

    if (CompilerInstance* CI = getCIOrNull())
      CI->getDiagnostics().getClient()->EndSourceFile();

    // LookupHelper's ~Parser needs the PP from IncrParser's CI, so do this
    // first:
    m_LookupHelper.reset();

    // We want to keep the callback alive during the shutdown of Sema, CodeGen
    // and the ASTContext. For that to happen we shut down the IncrementalParser
    // explicitly, before the implicit destruction (through the unique_ptr) of
    // the callbacks.
    m_IncrParser.reset(0);
    m_SYCLCompiler.reset(0);

  }

  Transaction* Interpreter::Initialize(bool NoRuntime, bool SyntaxOnly,
                              llvm::SmallVectorImpl<llvm::StringRef>& Globals) {
    largestream Strm;
    const clang::LangOptions& LangOpts = getCI()->getLangOpts();
    const void* ThisP = static_cast<void*>(this);
    // PCH/PCM-generation defines syntax-only. If we include definitions,
    // loading the PCH/PCM will make the runtime barf about dupe definitions.
    bool EmitDefinitions = !SyntaxOnly;

    // FIXME: gCling should be const so assignemnt is a compile time error.
    // Currently the name mangling is coming up wrong for the const version
    // (on OS X at least, so probably Linux too) and the JIT thinks the symbol
    // is undefined in a child Interpreter.  And speaking of children, should
    // gCling actually be thisCling, so a child Interpreter can only access
    // itself? One could use a macro (simillar to __dso_handle) to block
    // assignemnt and get around the mangling issue.
    const char* Linkage = LangOpts.CPlusPlus ? "extern \"C\"" : "";
    if (!NoRuntime) {
      if (LangOpts.CPlusPlus) {
        Strm << "#include \"cling/Interpreter/RuntimeUniverse.h\"\n";
        if (EmitDefinitions)
          Strm << "namespace cling { class Interpreter; namespace runtime { "
                  "Interpreter* gCling=(Interpreter*)" << ThisP << ";}}\n";
      } else {
        Strm << "#include \"cling/Interpreter/CValuePrinter.h\"\n"
             << "void* gCling";
        if (EmitDefinitions)
          Strm << "=(void*)" << ThisP;
        Strm << ";\n";
      }
    }

    // Intercept all atexit calls, as the Interpreter and functions will be long
    // gone when the -native- versions invoke them.
#if defined(__linux__)
    const char* LinkageCxx = "extern \"C++\"";
    const char* Attr = LangOpts.CPlusPlus ? " throw () " : "";
#else
    const char* LinkageCxx = Linkage;
    const char* Attr = "";
#endif

#if defined(__GLIBCXX__)
    const char* cxa_atexit_is_noexcept = LangOpts.CPlusPlus ? " noexcept" : "";
#else
    const char* cxa_atexit_is_noexcept = "";
#endif

    // While __dso_handle is still overriden in the JIT below,
    // #define __dso_handle is used to mitigate the following problems:
    //  1. Type of __dso_handle is void* making assignemnt to it legal
    //  2. Making it void* const in cling would mean possible type mismatch
    //  3. Cannot override void* __dso_handle in child Interpreter
    //  4. On Unix where the symbol actually exists, __dso_handle will be
    //     linked into the code before the JIT can say otherwise, so:
    //      [cling] __dso_handle // codegened __dso_handle always printed
    //      [cling] __cxa_atexit(f, 0, __dso_handle) // seg-fault
    //  5. Code that actually uses __dso_handle will fail as a declaration is
    //     needed which is not possible with the macro.
    //  6. Assuming 4 is sorted out in user code, calling __cxa_atexit through
    //     atexit below isn't linking to the __dso_handle symbol.

    // Use __cxa_atexit to intercept all of the following routines
    Strm << Linkage << " int __cxa_atexit(void (*f)(void*), void*, void*) "
         << cxa_atexit_is_noexcept << ";\n";

    if (EmitDefinitions)
      Strm << "#define __dso_handle ((void*)" << ThisP << ")\n";

    // C atexit, std::atexit
    Strm << Linkage << " int atexit(void(*f)()) " << Attr;
    if (EmitDefinitions)
      Strm << " { return __cxa_atexit((void(*)(void*))f, 0, __dso_handle); }\n";
    else
      Strm << ";\n";
    Globals.push_back("atexit");

    // C++ 11 at_quick_exit, std::at_quick_exit
    if (LangOpts.CPlusPlus && LangOpts.CPlusPlus11) {
      Strm << LinkageCxx << " int at_quick_exit(void(*f)()) " << Attr;
      if (EmitDefinitions)
        Strm
          << " { return __cxa_atexit((void(*)(void*))f, 0, __dso_handle); }\n";
      else
        Strm << ";\n";
      Globals.push_back("at_quick_exit");
    }

#if defined(LLVM_ON_WIN32)
    // Windows specific: _onexit, _onexit_m, __dllonexit
#if !defined(_M_CEE)
    const char* Spec = "__cdecl";
#else
    const char* Spec = "__clrcall";
#endif
    Strm << Linkage << " " << Spec << " int (*__dllonexit("
         << "int (" << Spec << " *f)(void**, void**), void**, void**))"
         "(void**, void**)";
      if (EmitDefinitions)
        Strm << " { __cxa_atexit((void(*)(void*))f, 0, __dso_handle);"
                " return f; }\n";
      else
        Strm << ";\n";
    Globals.push_back("__dllonexit");
#if !defined(_M_CEE_PURE)
    Strm << Linkage << " " << Spec << " int (*_onexit("
         << "int (" << Spec << " *f)()))()";
    if (EmitDefinitions)
      Strm << " { __cxa_atexit((void(*)(void*))f, 0, __dso_handle);"
              " return f; }\n";
    else
      Strm << ";\n";
    Globals.push_back("_onexit");
#endif
#endif

    if (!SyntaxOnly) {
      // Override the native symbols now, before anything can be emitted.
      m_Executor->addSymbol("__cxa_atexit",
                            utils::FunctionToVoidPtr(&local_cxa_atexit), true);
      // __dso_handle is inserted for the link phase, as macro is useless then
      m_Executor->addSymbol("__dso_handle", this, true);

#ifdef _MSC_VER
      // According to the PE Format spec, in "The .tls Section"
      // (http://www.microsoft.com/whdc/system/platform/firmware/PECOFF.mspx):
      //   2. When a thread is created, the loader communicates the address
      //   of the thread's TLS array by placing the address of the thread
      //   environment block (TEB) in the FS register. A pointer to the TLS
      //   array is at the offset of 0x2C from the beginning of TEB. This
      //   behavior is Intel x86-specific.
      static const unsigned long _tls_array = 0x2C;
      m_Executor->addSymbol("_tls_array", (void *)&_tls_array, true);
#endif

#ifdef CLING_WIN_SEH_EXCEPTIONS
      // Windows C++ SEH handler
      m_Executor->addSymbol("_CxxThrowException",
          utils::FunctionToVoidPtr(&platform::ClingRaiseSEHException), true);
#endif
    }

    if (m_Opts.Verbose())
      cling::errs() << Strm.str();

    Transaction *T;
    declare(Strm.str(), &T);
    return T;
  }

  void Interpreter::AddIncludePaths(llvm::StringRef PathStr, const char* Delm) {
    CompilerInstance* CI = getCI();
    HeaderSearchOptions& HOpts = CI->getHeaderSearchOpts();

    // Save the current number of entries
    size_t Idx = HOpts.UserEntries.size();
    utils::AddIncludePaths(PathStr, HOpts, Delm);

    Preprocessor& PP = CI->getPreprocessor();
    SourceManager& SM = PP.getSourceManager();
    FileManager& FM = SM.getFileManager();
    HeaderSearch& HSearch = PP.getHeaderSearchInfo();
    const bool isFramework = false;

    // Add all the new entries into Preprocessor
    for (const size_t N = HOpts.UserEntries.size(); Idx < N; ++Idx) {
      const HeaderSearchOptions::Entry& E = HOpts.UserEntries[Idx];
      m_SYCLCompiler->addCompileArg("-I", E.Path);
      if (const clang::DirectoryEntry *DE = FM.getDirectory(E.Path)) {
        HSearch.AddSearchPath(DirectoryLookup(DE, SrcMgr::C_User, isFramework),
                              E.Group == frontend::Angled);
      }
    }
  }

  void Interpreter::AddIncludePath(llvm::StringRef PathsStr) {
    return AddIncludePaths(PathsStr, nullptr);
  }

  void Interpreter::DumpIncludePath(llvm::raw_ostream* S) {
    utils::DumpIncludePaths(getCI()->getHeaderSearchOpts(), S ? *S : cling::outs(),
                            true /*withSystem*/, true /*withFlags*/);
  }

  // FIXME: Add stream argument and move DumpIncludePath path here.
  void Interpreter::dump(llvm::StringRef what, llvm::StringRef filter) {
    llvm::raw_ostream &where = cling::log();
    // `.stats decl' and `.stats asttree FILTER' cause deserialization; force transaction
    PushTransactionRAII RAII(this);
    if (what.equals("asttree")) {
      std::unique_ptr<clang::ASTConsumer> printer =
          clang::CreateASTDumper(filter, true  /*DumpDecls*/,
                                         false /*Deserialize*/,
                                         false /*DumpLookups*/);
      printer->HandleTranslationUnit(getSema().getASTContext());
    } else if (what.equals("ast"))
      getSema().getASTContext().PrintStats();
    else if (what.equals("decl"))
      ClangInternalState::printLookupTables(where, getSema().getASTContext());
    else if (what.equals("undo"))
      m_IncrParser->printTransactionStructure();
  }

  void Interpreter::storeInterpreterState(const std::string& name) const {
    // This may induce deserialization
    PushTransactionRAII RAII(this);
    CodeGenerator* CG = m_IncrParser->getCodeGenerator();
    ClangInternalState* state = new ClangInternalState(
        getCI()->getASTContext(), getCI()->getPreprocessor(),
        getLastTransaction()->getModule().get(), CG, name);
    m_StoredStates.push_back(state);
  }

  void Interpreter::compareInterpreterState(const std::string &Name) const {
    const auto Itr = std::find_if(
        m_StoredStates.begin(), m_StoredStates.end(),
        [&Name](const ClangInternalState *S) { return S->getName() == Name; });
    if (Itr == m_StoredStates.end()) {
      cling::errs() << "The store point name " << Name
                    << " does not exist."
                       "Unbalanced store / compare\n";
      return;
    }
    // This may induce deserialization
    PushTransactionRAII RAII(this);
    (*Itr)->compare(Name, m_Opts.Verbose());
  }

  void Interpreter::printIncludedFiles(llvm::raw_ostream& Out) const {
    ClangInternalState::printIncludedFiles(Out, getCI()->getSourceManager());
  }

  namespace valuePrinterInternal {
    void declarePrintValue(Interpreter &Interp);
  }

  std::string Interpreter::toString(const char* type, void* obj) {
    LockCompilationDuringUserCodeExecutionRAII LCDUCER(*this);
    cling::valuePrinterInternal::declarePrintValue(*this);
    std::string buf, ret;
    llvm::raw_string_ostream ss(buf);
    ss << "*((std::string*)" << &ret << ") = cling::printValue((" << type << "*)"
       << obj << ");";
    CompilationResult result = process(ss.str().c_str());
    if (result != cling::Interpreter::kSuccess)
      llvm::errs() << "Error in Interpreter::toString: the input " << ss.str()
                   << " cannot be evaluated";

    return ret;
  }

  void Interpreter::GetIncludePaths(llvm::SmallVectorImpl<std::string>& incpaths,
                                   bool withSystem, bool withFlags) {
    utils::CopyIncludePaths(getCI()->getHeaderSearchOpts(), incpaths,
                            withSystem, withFlags);
  }

  CompilerInstance* Interpreter::getCI() const {
    return m_IncrParser->getCI();
  }

  CompilerInstance* Interpreter::getCIOrNull() const {
    return m_IncrParser ? m_IncrParser->getCI() : nullptr;
  }

  Sema& Interpreter::getSema() const {
    return getCI()->getSema();
  }

  DiagnosticsEngine& Interpreter::getDiagnostics() const {
    return getCI()->getDiagnostics();
  }

  CompilationOptions Interpreter::makeDefaultCompilationOpts() const {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.EnableShadowing = 0;
    CO.ValuePrinting = CompilationOptions::VPDisabled;
    CO.CodeGeneration = m_IncrParser->hasCodeGenerator();
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingDebug();
    CO.IgnorePromptDiags = !isRawInputEnabled();
    CO.CheckPointerValidity = !isRawInputEnabled();
    CO.OptLevel = getDefaultOptLevel();
    return CO;
  }

  const MacroInfo* Interpreter::getMacro(llvm::StringRef Macro) const {
    clang::Preprocessor& PP = getCI()->getPreprocessor();
    if (IdentifierInfo* II = PP.getIdentifierInfo(Macro)) {
      // If the information about this identifier is out of date, update it from
      // the external source.
      // FIXME: getIdentifierInfo will probably do this for us once we update
      // clang. If so, please remove this manual update.
      if (II->isOutOfDate())
        PP.getExternalSource()->updateOutOfDateIdentifier(*II);
      MacroDefinition MDef = PP.getMacroDefinition(II);
      MacroInfo* MI = MDef.getMacroInfo();
      return MI;
    }
    return nullptr;
  }

  std::string Interpreter::getMacroValue(llvm::StringRef Macro,
                                         const char* Trim) const {
    std::string Value;
    if (const MacroInfo* MI = getMacro(Macro)) {
      for (const clang::Token& Tok : MI->tokens()) {
        llvm::SmallString<64> Buffer;
        Macro = getCI()->getPreprocessor().getSpelling(Tok, Buffer);
        if (!Value.empty())
          Value += " ";
        Value += Trim ? Macro.trim(Trim).str() : Macro.str();
      }
    }
    return Value;
  }

  ///\brief Maybe transform the input line to implement cint command line
  /// semantics (declarations are global) and compile to produce a module.
  ///
  Interpreter::CompilationResult
  Interpreter::process(const std::string& input, Value* V /* = 0 */,
                       Transaction** T /* = 0 */,
                       bool disableValuePrinting /* = false*/) {
    std::string wrapReadySource = input;
    size_t wrapPoint = std::string::npos;
    m_SYCLCompiler->setExtractDeclFlag(true);
    if (!isRawInputEnabled())
      wrapPoint = utils::getWrapPoint(wrapReadySource, getCI()->getLangOpts());

    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.EnableShadowing = m_RedefinitionAllowed && !isRawInputEnabled();
    if (isRawInputEnabled() || wrapPoint == std::string::npos) {
      CO.DeclarationExtraction = 0;
      CO.ValuePrinting = 0;
      CO.ResultEvaluation = 0;
      return DeclareInternal(input, CO, T);
    }

    CO.DeclarationExtraction = 1;
    CO.ValuePrinting = disableValuePrinting ? CompilationOptions::VPDisabled
      : CompilationOptions::VPAuto;
    CO.ResultEvaluation = (bool)V;
    // CO.IgnorePromptDiags = 1; done by EvaluateInternal().
    CO.CheckPointerValidity = 1;
    if (EvaluateInternal(wrapReadySource, CO, V, T, wrapPoint)
                                                     == Interpreter::kFailure) {
      return Interpreter::kFailure;
    }

    return Interpreter::kSuccess;
  }

  Interpreter::CompilationResult
  Interpreter::parse(const std::string& input, Transaction** T /*=0*/) const {
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.CodeGeneration = 0;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;

    return DeclareInternal(input, CO, T);
  }

  ///\returns true if the module was loaded.
  bool Interpreter::loadModule(const std::string& moduleName,
                               bool complain /*= true*/) {
    assert(getCI()->getLangOpts().Modules
           && "Function only relevant when C++ modules are turned on!");

    Preprocessor& PP = getCI()->getPreprocessor();
    HeaderSearch &HS = PP.getHeaderSearchInfo();

    if (Module *M = HS.lookupModule(moduleName, /*AllowSearch*/true,
                                    /*AllowExtraSearch*/ true))
      return loadModule(M, complain);

   if (complain)
     llvm::errs() << "Module " << moduleName << " not found.\n";


   return false;
  }

  bool Interpreter::loadModule(clang::Module* M, bool complain /* = true*/) {
    assert(getCI()->getLangOpts().Modules
           && "Function only relevant when C++ modules are turned on!");
    assert(M && "Module missing");
    if (getSema().isModuleVisible(M))
      return true;

    // FIXME: What about importing submodules such as std.blah. This disables
    // this functionality.
    if (declare("#pragma clang module import \"" + M->Name + "\"") == kSuccess)
      return true;

    if (complain) {
      if (M->IsSystem)
        llvm::errs() << "Failed to load module " << M->Name << "\n";
      else
        llvm::outs() << "Failed to load module " << M->Name << "\n";
    }

   return false;
  }

  Interpreter::CompilationResult
  Interpreter::parseForModule(const std::string& input) {
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.CodeGenerationForModule = 1;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;

    // When doing parseForModule avoid warning about the user code
    // being loaded ... we probably might as well extend this to
    // ALL warnings ... but this will suffice for now (working
    // around a real bug in QT :().
    DiagnosticsEngine& Diag = getDiagnostics();
    Diag.setSeverity(clang::diag::warn_field_is_uninit,
                     clang::diag::Severity::Ignored, SourceLocation());
    CompilationResult Result = DeclareInternal(input, CO);
    Diag.setSeverity(clang::diag::warn_field_is_uninit,
                     clang::diag::Severity::Warning, SourceLocation());
    return Result;
  }


  Interpreter::CompilationResult
  Interpreter::CodeCompleteInternal(const std::string& input, unsigned offset) {

    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.CheckPointerValidity = 0;

    std::string wrapped = input;
    size_t wrapPos = utils::getWrapPoint(wrapped, getCI()->getLangOpts());
    const std::string& Src = WrapInput(wrapped, wrapped, wrapPos);

    CO.CodeCompletionOffset = offset + wrapPos;

    StateDebuggerRAII stateDebugger(this);

    // This triggers the FileEntry to be created and the completion
    // point to be set in clang.
    m_IncrParser->Compile(Src, CO);

    return kSuccess;
  }

  Interpreter::CompilationResult
  Interpreter::declare(const std::string& input, Transaction** T/*=0 */) {
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.CheckPointerValidity = 0;

    return DeclareInternal(input, CO, T);
  }

  Interpreter::CompilationResult
  Interpreter::evaluate(const std::string& input, Value& V) {
    // Here we might want to enforce further restrictions like: Only one
    // ExprStmt can be evaluated and etc. Such enforcement cannot happen in the
    // worker, because it is used from various places, where there is no such
    // rule
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 1;
    CO.CheckPointerValidity = 0;

    return EvaluateInternal(input, CO, &V);
  }

  Interpreter::CompilationResult
  Interpreter::codeComplete(const std::string& line, size_t& cursor,
                            std::vector<std::string>& completions) const {

    const char * const argV = "cling";
    std::string resourceDir = this->getCI()->getHeaderSearchOpts().ResourceDir;
    // Remove the extra 3 directory names "/lib/clang/3.9.0"
    StringRef parentResourceDir = llvm::sys::path::parent_path(
                                  llvm::sys::path::parent_path(
                                  llvm::sys::path::parent_path(resourceDir)));
    std::string llvmDir = parentResourceDir.str();

    Interpreter childInterpreter(*this, 1, &argV, llvmDir.c_str());
    if (!childInterpreter.isValid())
      return kFailure;

    auto childCI = childInterpreter.getCI();
    clang::Sema &childSemaRef = childCI->getSema();

    // Create the CodeCompleteConsumer with InterpreterCallbacks
    // from the parent interpreter and set the consumer for the child
    // interpreter.
    ClingCodeCompleteConsumer* consumer = new ClingCodeCompleteConsumer(
                getCI()->getFrontendOpts().CodeCompleteOpts, completions);
    // Child interpreter CI will own consumer!
    childCI->setCodeCompletionConsumer(consumer);
    childSemaRef.CodeCompleter = consumer;

    // Ignore diagnostics when we tab complete.
    // This is because we get redefinition errors due to the import of the decls.
    clang::IgnoringDiagConsumer* ignoringDiagConsumer =
                                            new clang::IgnoringDiagConsumer();
    childSemaRef.getDiagnostics().setClient(ignoringDiagConsumer, true);
    DiagnosticsEngine& parentDiagnostics = this->getCI()->getSema().getDiagnostics();

    std::unique_ptr<DiagnosticConsumer> ownerDiagConsumer =
                                                parentDiagnostics.takeClient();
    auto clientDiagConsumer = parentDiagnostics.getClient();
    parentDiagnostics.setClient(ignoringDiagConsumer, /*owns*/ false);

    // The child will desirialize decls from *this. We need a transaction RAII.
    PushTransactionRAII RAII(this);

    // Triger the code completion.
    childInterpreter.CodeCompleteInternal(line, cursor);

    // Restore the original diagnostics client for parent interpreter.
    parentDiagnostics.setClient(clientDiagConsumer,
                                ownerDiagConsumer.release() != nullptr);
    parentDiagnostics.Reset(/*soft=*/true);

    return kSuccess;
  }

  Interpreter::CompilationResult
  Interpreter::echo(const std::string& input, Value* V /* = 0 */) {
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = CompilationOptions::VPEnabled;
    CO.ResultEvaluation = (bool)V;

    return EvaluateInternal(input, CO, V);
  }

  Interpreter::CompilationResult
  Interpreter::execute(const std::string& input) {
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = 0;
    return EvaluateInternal(input, CO);
  }

  Interpreter::CompilationResult Interpreter::emitAllDecls(Transaction* T) {
    assert(!isInSyntaxOnlyMode() && "No CodeGenerator?");
    m_IncrParser->emitTransaction(T);
    m_IncrParser->addTransaction(T);
    T->setState(Transaction::kCollecting);
    auto PRT = m_IncrParser->endTransaction(T);
    m_IncrParser->commitTransaction(PRT);

    if ((T = PRT.getPointer()))
      if (executeTransaction(*T))
        return Interpreter::kSuccess;

    return Interpreter::kFailure;
  }

  static void makeUniqueName(llvm::raw_ostream &Strm, unsigned long long ID) {
    Strm << utils::Synthesize::UniquePrefix << ID;
  }

  static std::string makeUniqueWrapper(unsigned long long ID) {
    cling::ostrstream Strm;
    Strm << "void ";
    makeUniqueName(Strm, ID);
    Strm << "(void* vpClingValue) {\n ";
    return Strm.str();
  }

  void Interpreter::createUniqueName(std::string &Out) {
    llvm::raw_string_ostream Strm(Out);
    makeUniqueName(Strm, m_UniqueCounter++);
  }

  bool Interpreter::isUniqueName(llvm::StringRef name) {
    return name.startswith(utils::Synthesize::UniquePrefix);
  }

  clang::SourceLocation Interpreter::getSourceLocation(bool skipWrapper) const {
    const Transaction* T = getLatestTransaction();
    if (!T)
      return SourceLocation();

    const SourceManager &SM = getCI()->getSourceManager();
    if (skipWrapper) {
      return T->getSourceStart(SM).getLocWithOffset(
          makeUniqueWrapper(m_UniqueCounter).size());
    }
    return T->getSourceStart(SM);
  }

  const std::string& Interpreter::WrapInput(const std::string& Input,
                                            std::string& Output,
                                            size_t& WrapPoint) const {
    // If wrapPoint is > length of input, nothing is wrapped!
    if (WrapPoint < Input.size()) {
      const std::string Header = makeUniqueWrapper(m_UniqueCounter++);

      // Suppport Input and Output begin the same string
      std::string Wrapper = Input.substr(WrapPoint);
      Wrapper.insert(0, Header);
      Wrapper.append("\n;\n}");
      Wrapper.insert(0, Input.substr(0, WrapPoint));
      //printf("line:%s wp:%d wrapper:%s\n",Input.c_str(),WrapPoint,Wrapper.c_str());
      Wrapper.swap(Output);
      WrapPoint += Header.size();
      return Output;
    }
    // in-case std::string::npos was passed
    WrapPoint = 0;
    return Input;
  }

  Interpreter::ExecutionResult
  Interpreter::RunFunction(const FunctionDecl* FD, Value* res /*=0*/) {
    if (getDiagnostics().hasErrorOccurred())
      return kExeCompilationError;

    if (isInSyntaxOnlyMode()) {
      return kExeNoCodeGen;
    }

    if (!FD)
      return kExeUnkownFunction;

    std::string mangledNameIfNeeded;
    utils::Analyze::maybeMangleDeclName(FD, mangledNameIfNeeded);
    IncrementalExecutor::ExecutionResult ExeRes =
       m_Executor->executeWrapper(mangledNameIfNeeded, res);
    return ConvertExecutionResult(ExeRes);
  }

  const FunctionDecl* Interpreter::DeclareCFunction(StringRef name,
                                                    StringRef code,
                                                    bool withAccessControl,
                                                    Transaction*& T) {
    /*
    In CallFunc we currently always (intentionally and somewhat necessarily)
    always fully specify member function template, however this can lead to
    an ambiguity with a class template.  For example in
    roottest/cling/functionTemplate we get:

    input_line_171:3:15: warning: lookup of 'set' in member access expression
    is ambiguous; using member of 't'
    ((t*)obj)->set<int>(*(int*)args[0]);
               ^
    roottest/cling/functionTemplate/t.h:19:9: note: lookup in the object type
    't' refers here
    void set(T targ) {
         ^
    /usr/include/c++/4.4.5/bits/stl_set.h:87:11: note: lookup from the
    current scope refers here
    class set
          ^
    This is an intention warning implemented in clang, see
    http://llvm.org/viewvc/llvm-project?view=revision&revision=105518

    which 'should have been' an error:

    C++ [basic.lookup.classref] requires this to be an error, but,
    because it's hard to work around, Clang downgrades it to a warning as
    an extension.</p>

    // C++98 [basic.lookup.classref]p1:
    // In a class member access expression (5.2.5), if the . or -> token is
    // immediately followed by an identifier followed by a <, the identifier
    // must be looked up to determine whether the < is the beginning of a
    // template argument list (14.2) or a less-than operator. The identifier
    // is first looked up in the class of the object expression. If the
    // identifier is not found, it is then looked up in the context of the
    // entire postfix-expression and shall name a class or function template. If
    // the lookup in the class of the object expression finds a template, the
    // name is also looked up in the context of the entire postfix-expression
    // and
    // -- if the name is not found, the name found in the class of the
    // object expression is used, otherwise
    // -- if the name is found in the context of the entire postfix-expression
    // and does not name a class template, the name found in the class of the
    // object expression is used, otherwise
    // -- if the name found is a class template, it must refer to the same
    // entity as the one found in the class of the object expression,
    // otherwise the program is ill-formed.

    See -Wambiguous-member-template

    An alternative to disabling the diagnostics is to use a pointer to
    member function:

    #include <set>
    using namespace std;

    extern "C" int printf(const char*,...);

    struct S {
    template <typename T>
    void set(T) {};

    virtual void virtua() { printf("S\n"); }
    };

    struct T: public S {
    void virtua() { printf("T\n"); }
    };

    int main() {
    S *s = new T();
    typedef void (S::*Func_p)(int);
    Func_p p = &S::set<int>;
    (s->*p)(12);

    typedef void (S::*Vunc_p)(void);
    Vunc_p q = &S::virtua;
    (s->*q)(); // prints "T"
    return 0;
    }
    */
    DiagnosticsEngine& Diag = getDiagnostics();
    Diag.setSeverity(clang::diag::ext_nested_name_member_ref_lookup_ambiguous,
                     clang::diag::Severity::Ignored, SourceLocation());


    LangOptions& LO = const_cast<LangOptions&>(getCI()->getLangOpts());
    bool savedAccessControl = LO.AccessControl;
    LO.AccessControl = withAccessControl;
    T = nullptr;
    cling::Interpreter::CompilationResult CR = declare(code, &T);
    LO.AccessControl = savedAccessControl;

    Diag.setSeverity(clang::diag::ext_nested_name_member_ref_lookup_ambiguous,
                     clang::diag::Severity::Warning, SourceLocation());

    if (CR != cling::Interpreter::kSuccess)
      return 0;

    for (cling::Transaction::const_iterator I = T->decls_begin(),
           E = T->decls_end(); I != E; ++I) {
      if (I->m_Call != cling::Transaction::kCCIHandleTopLevelDecl)
        continue;
      if (const LinkageSpecDecl* LSD
          = dyn_cast<LinkageSpecDecl>(*I->m_DGR.begin())) {
        DeclContext::decl_iterator DeclBegin = LSD->decls_begin();
        if (DeclBegin == LSD->decls_end())
          continue;
        if (const FunctionDecl* D = dyn_cast<FunctionDecl>(*DeclBegin)) {
          const IdentifierInfo* II = D->getDeclName().getAsIdentifierInfo();
          if (II && II->getName() == name)
            return D;
        }
      }
    }
    return 0;
  }

  void*
  Interpreter::compileFunction(llvm::StringRef name, llvm::StringRef code,
                               bool ifUnique, bool withAccessControl) {
    //
    //  Compile the wrapper code.
    //

    if (isInSyntaxOnlyMode())
      return 0;

    if (ifUnique) {
      if (void* Addr = (void*)getAddressOfGlobal(name)) {
        return Addr;
      }
    }

    Transaction* T = nullptr;
    const FunctionDecl* FD = DeclareCFunction(name, code, withAccessControl, T);
    if (!FD || !T)
      return 0;
    //
    //  Get the wrapper function pointer
    //  from the ExecutionEngine (the JIT).
    //
    if (const llvm::GlobalValue* GV
        = T->getModule()->getNamedValue(name))
      return m_Executor->getPointerToGlobalFromJIT(*GV);

    return 0;
  }

  void*
  Interpreter::compileDtorCallFor(const clang::RecordDecl* RD) {
    void* &addr = m_DtorWrappers[RD];
    if (addr)
      return addr;

    if (const CXXRecordDecl *CXX = dyn_cast<CXXRecordDecl>(RD)) {
      // Don't generate a stub for a destructor that does nothing
      // This also fixes printing of lambdas and C structures as they
      // have no dtor test/ValuePrinter/Destruction.C
      if (CXX->hasIrrelevantDestructor())
        return nullptr;
    }

    smallstream funcname;
    funcname << "__cling_Destruct_" << RD;

    largestream code;
    code << "extern \"C\" void " << funcname.str() << "(void* obj){(("
         << utils::TypeName::GetFullyQualifiedName(
                clang::QualType(RD->getTypeForDecl(), 0), RD->getASTContext())
         << "*)obj)->~" << RD->getNameAsString() << "();}";

    // ifUniq = false: we know it's unique, no need to check.
    addr = compileFunction(funcname.str(), code.str(), false /*ifUniq*/,
                           false /*withAccessControl*/);
    return addr;
  }

  Interpreter::CompilationResult
  Interpreter::DeclareInternal(const std::string& input,
                               const CompilationOptions& CO,
                               Transaction** T /* = 0 */) const {
    assert(CO.DeclarationExtraction == 0
           && CO.ValuePrinting == 0
           && CO.ResultEvaluation == 0
           && "Compilation Options not compatible with \"declare\" mode.");

    StateDebuggerRAII stateDebugger(this);
    if (!m_SYCLCompiler->compile(input, NULL, 0, std::string::npos)){
      return kFailure;
    }
    
    IncrementalParser::ParseResultTransaction PRT
      = m_IncrParser->Compile(input, CO);
    if (PRT.getInt() == IncrementalParser::kFailed)
      return Interpreter::kFailure;
    
    m_SYCLCompiler->setDeclSuccess(PRT.getPointer());
    
    if (T)
      *T = PRT.getPointer();
    return Interpreter::kSuccess;
  }

  Interpreter::CompilationResult
  Interpreter::EvaluateInternal(const std::string& input,
                                CompilationOptions CO,
                                Value* V, /* = 0 */
                                Transaction** T /* = 0 */,
                                size_t wrapPoint /* = 0*/) {
    StateDebuggerRAII stateDebugger(this);
    if (!m_SYCLCompiler->compile(input, NULL, 1, wrapPoint)){
      return kFailure;
    }

    // Wrap the expression
    std::string WrapperBuffer;
    const std::string& Wrapper = WrapInput(input, WrapperBuffer, wrapPoint);

    // We have wrapped and need to disable warnings that are caused by
    // non-default C++ at the prompt:
    CO.IgnorePromptDiags = 1;
    //printf("compiled_wrapper:%s\n",Wrapper.c_str());
    
    IncrementalParser::ParseResultTransaction PRT
      = m_IncrParser->Compile(Wrapper, CO);
    Transaction* lastT = PRT.getPointer();
    //lastT->printStructure();
    m_SYCLCompiler->setTransaction(lastT);
    
    if (lastT && lastT->getState() != Transaction::kCommitted) {
      assert((lastT->getState() == Transaction::kCommitted
              || lastT->getState() == Transaction::kRolledBack
              || lastT->getState() == Transaction::kRolledBackWithErrors)
             && "Not committed?");
      if (V)
        *V = Value();
      return kFailure;
    }

    // Might not have a Transaction
    if (PRT.getInt() == IncrementalParser::kFailed) {
      if (V)
        *V = Value();
      return kFailure;
    }

    if (!lastT) {
      // Empty transactions are good, too!
      if (V)
        *V = Value();
      return kSuccess;
    }

    Value resultV;
    if (!V)
      V = &resultV;
    if (!lastT->getWrapperFD()) {// no wrapper to run 
      return Interpreter::kSuccess;
    }
    else {
      ExecutionResult res = RunFunction(lastT->getWrapperFD(), V);
      if (res < kExeFirstError) {
         if (lastT->getCompilationOpts().ValuePrinting
            != CompilationOptions::VPDisabled
            && V->isValid()
            // the !V->needsManagedAllocation() case is handled by
            // dumpIfNoStorage.
            && V->needsManagedAllocation())
         V->dump();
         return Interpreter::kSuccess;
      } else {
        return Interpreter::kFailure;
      }
    }
    return Interpreter::kSuccess;
  }

  std::string Interpreter::lookupFileOrLibrary(llvm::StringRef file) {
    //printf("loader:%s\n",file.str().c_str());
    std::string canonicalFile = DynamicLibraryManager::normalizePath(file);
    if (canonicalFile.empty())
      canonicalFile = file;
    const FileEntry* FE = 0;

    //printf("file:%s\n",canonicalFile.c_str());
    //Copied from clang's PPDirectives.cpp
    bool isAngled = false;
    // Clang doc says:
    // "LookupFrom is set when this is a \#include_next directive, it
    // specifies the file to start searching from."
    const DirectoryLookup* FromDir = 0;
    const FileEntry* FromFile = 0;
    const DirectoryLookup* CurDir = 0;
    Preprocessor& PP = getCI()->getPreprocessor();
    // PP::LookupFile uses it to issue 'nice' diagnostic
    SourceLocation fileNameLoc;
    FE = PP.LookupFile(fileNameLoc, canonicalFile, isAngled, FromDir, FromFile,
                       CurDir, /*SearchPath*/0, /*RelativePath*/ 0,
                       /*suggestedModule*/0, 0 /*IsMapped*/, /*SkipCache*/false,
                       /*OpenFile*/ false, /*CacheFail*/ false);
    if (FE)
      return FE->getName();
    return getDynamicLibraryManager()->lookupLibrary(canonicalFile);
  }

  Interpreter::CompilationResult
  Interpreter::loadLibrary(const std::string& filename, bool lookup) {
    //printf("library:%s\n",filename.c_str());
    DynamicLibraryManager* DLM = getDynamicLibraryManager();
    std::string canonicalLib;
    if (lookup)
      canonicalLib = DLM->lookupLibrary(filename);

    const std::string &library = lookup ? canonicalLib : filename;
    if (!library.empty()) {
      switch (DLM->loadLibrary(library, /*permanent*/false, /*resolved*/true)) {
      case DynamicLibraryManager::kLoadLibSuccess: // Intentional fall through
      case DynamicLibraryManager::kLoadLibAlreadyLoaded:
        return kSuccess;
      case DynamicLibraryManager::kLoadLibNotFound:
        assert(0 && "Cannot find library with existing canonical name!");
        return kFailure;
      default:
        // Not a source file (canonical name is non-empty) but can't load.
        return kFailure;
      }
    }
    return kMoreInputExpected;
  }

  Interpreter::CompilationResult
  Interpreter::loadHeader(const std::string& filename,
                          Transaction** T /*= 0*/) {
    std::string code;
    code += "#include \"" + filename + "\"";
    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.CheckPointerValidity = 1;
    CompilationResult res = DeclareInternal(code, CO, T);
    return res;
  }

  void Interpreter::unload(Transaction& T) {
    // Clear any stored states that reference the llvm::Module.
    // Do it first in case
    m_SYCLCompiler->removeCodeByTransaction(&T);
    auto Module = T.getModule();
    if (Module && !m_StoredStates.empty()) {
      const auto Predicate = [&Module](const ClangInternalState* S) {
        return S->getModule() == Module.get();
      };
      auto Itr =
          std::find_if(m_StoredStates.begin(), m_StoredStates.end(), Predicate);
      while (Itr != m_StoredStates.end()) {
        if (m_Opts.Verbose()) {
          cling::errs() << "Unloading Transaction forced state '"
                        << (*Itr)->getName() << "' to be destroyed\n";
        }
        m_StoredStates.erase(Itr);

        Itr = std::find_if(m_StoredStates.begin(), m_StoredStates.end(),
                           Predicate);
      }
    }

    // Clear any cached transaction states.
    for (unsigned i = 0; i < kNumTransactions; ++i) {
      if (m_CachedTrns[i] == &T) {
        m_CachedTrns[i] = nullptr;
        break;
      }
    }

    if (InterpreterCallbacks* callbacks = getCallbacks())
      callbacks->TransactionUnloaded(T);
    if (m_Executor) // we also might be in fsyntax-only mode.
      m_Executor->runAndRemoveStaticDestructors(&T);

    // We can revert the most recent transaction or a nested transaction of a
    // transaction that is not in the middle of the transaction collection
    // (i.e. at the end or not yet added to the collection at all).
    assert(!T.getTopmostParent()->getNext() &&
           "Can not revert previous transactions");
    assert((T.getState() != Transaction::kRolledBack ||
            T.getState() != Transaction::kRolledBackWithErrors) &&
           "Transaction already rolled back.");
    if (getOptions().ErrorOut)
      return;

    if (InterpreterCallbacks* callbacks = getCallbacks())
      callbacks->TransactionRollback(T);

    TransactionUnloader U(this, &getCI()->getSema(),
                          m_IncrParser->getCodeGenerator(),
                          m_Executor.get());
    if (U.RevertTransaction(&T))
      T.setState(Transaction::kRolledBack);
    else
      T.setState(Transaction::kRolledBackWithErrors);

    m_IncrParser->deregisterTransaction(T);
  }

  void Interpreter::unload(unsigned numberOfTransactions) {
    const Transaction *First = m_IncrParser->getFirstTransaction();
    if (!First) {
      cling::errs() << "cling: No transactions to unload!";
      return;
    }
    for (unsigned i = 0; i < numberOfTransactions; ++i) {
      cling::Transaction* T = m_IncrParser->getLastTransaction();
      if (T == First) {
        cling::errs() << "cling: Can't unload first transaction!  Unloaded "
                      << i << " of " << numberOfTransactions << "\n";
        return;
      }
      unload(*T);
    }
  }

  Interpreter::CompilationResult
  Interpreter::loadFile(const std::string& filename,
                        bool allowSharedLib /*=true*/,
                        Transaction** T /*= 0*/) {
    if (allowSharedLib) {
      CompilationResult result = loadLibrary(filename, true);
      if (result!=kMoreInputExpected)
        return result;
    }
    return loadHeader(filename, T);
  }

  static void runAndRemoveStaticDestructorsImpl(IncrementalExecutor &executor,
                                std::vector<const Transaction*> &transactions,
                                         unsigned int begin, unsigned int end) {

    for(auto i = begin; i != end; --i) {
      if (transactions[i-1] != nullptr)
        executor.runAndRemoveStaticDestructors(const_cast<Transaction*>(transactions[i-1]));
    }
  }

  void Interpreter::runAndRemoveStaticDestructors(unsigned numberOfTransactions) {
    if (!m_Executor)
      return;
    auto transactions( m_IncrParser->getAllTransactions() );
    unsigned int min = 0;
    if (transactions.size() > numberOfTransactions) {
      min = transactions.size() - numberOfTransactions;
    }
    runAndRemoveStaticDestructorsImpl(*m_Executor, transactions,
                                      transactions.size(), min);
  }

  void Interpreter::runAndRemoveStaticDestructors() {
    if (!m_Executor)
      return;
    auto transactions( m_IncrParser->getAllTransactions() );
    runAndRemoveStaticDestructorsImpl(*m_Executor, transactions,
                                      transactions.size(), 0);
  }

  void Interpreter::installLazyFunctionCreator(void* (*fp)(const std::string&)) {
    if (m_Executor)
      m_Executor->installLazyFunctionCreator(fp);
  }

  Value Interpreter::Evaluate(const char* expr, DeclContext* DC,
                                       bool ValuePrinterReq) {
    Sema& TheSema = getCI()->getSema();
    // The evaluation should happen on the global scope, because of the wrapper
    // that is created.
    //
    // We can't PushDeclContext, because we don't have scope.
    Sema::ContextRAII pushDC(TheSema,
                             TheSema.getASTContext().getTranslationUnitDecl());

    Value Result;
    getCallbacks()->SetIsRuntime(true);
    if (ValuePrinterReq)
      echo(expr, &Result);
    else
      evaluate(expr, Result);
    getCallbacks()->SetIsRuntime(false);

    return Result;
  }

  void Interpreter::setCallbacks(std::unique_ptr<InterpreterCallbacks> C) {
    // We need it to enable LookupObject callback.
    if (!m_Callbacks) {
      m_Callbacks.reset(new MultiplexInterpreterCallbacks(this));
      // FIXME: Move to the InterpreterCallbacks.cpp;
      if (DynamicLibraryManager* DLM = getDynamicLibraryManager())
        DLM->setCallbacks(m_Callbacks.get());
      if (m_Executor)
        m_Executor->setCallbacks(m_Callbacks.get());
    }

    static_cast<MultiplexInterpreterCallbacks*>(m_Callbacks.get())
      ->addCallback(std::move(C));
  }

  const Transaction* Interpreter::getFirstTransaction() const {
    return m_IncrParser->getFirstTransaction();
  }

  const Transaction* Interpreter::getLastTransaction() const {
    return m_IncrParser->getLastTransaction();
  }

  const Transaction* Interpreter::getLastWrapperTransaction() const {
    return m_IncrParser->getLastWrapperTransaction();
  }
  const Transaction* Interpreter::getCurrentTransaction() const {
    return m_IncrParser->getCurrentTransaction();
  }

  const Transaction* Interpreter::getLatestTransaction() const {
    if (const Transaction* T = m_IncrParser->getCurrentTransaction())
      return T;
    return m_IncrParser->getLastTransaction();
  }

  void Interpreter::enableDynamicLookup(bool value /*=true*/) {
    if (!m_DynamicLookupDeclared && value) {
      // No dynlookup for the dynlookup header!
      m_DynamicLookupEnabled = false;
      declare("#include \"cling/Interpreter/DynamicLookupRuntimeUniverse.h\"");
    }
    m_DynamicLookupDeclared = true;

    // Enable it *after* parsing the headers.
    m_DynamicLookupEnabled = value;
  }

  Interpreter::ExecutionResult
  Interpreter::executeTransaction(Transaction& T) {
    assert(!isInSyntaxOnlyMode() && "Running on what?");
    assert(T.getState() == Transaction::kCommitted && "Must be committed");

    const std::shared_ptr<llvm::Module>& M = T.getModule();
    if (!M)
      return Interpreter::kExeNoModule;

    IncrementalExecutor::ExecutionResult ExeRes
       = IncrementalExecutor::kExeSuccess;
    if (!isPracticallyEmptyModule(M.get())) {
      m_Executor->emitModule(M, T.getCompilationOpts().OptLevel);

      // Forward to IncrementalExecutor; should not be called by
      // anyone except for IncrementalParser.
      ExeRes = m_Executor->runStaticInitializersOnce(T);
    }

    return ConvertExecutionResult(ExeRes);
  }

  bool Interpreter::addSymbol(const char* symbolName,  void* symbolAddress) {
    // Forward to IncrementalExecutor;
    if (!symbolName || !symbolAddress )
      return false;

    return m_Executor->addSymbol(symbolName, symbolAddress);
  }

  void* Interpreter::getAddressOfGlobal(const GlobalDecl& GD,
                                        bool* fromJIT /*=0*/) const {
    // Return a symbol's address, and whether it was jitted.
    std::string mangledName;
    utils::Analyze::maybeMangleDeclName(GD, mangledName);
#if defined(LLVM_ON_WIN32)
    // For some unknown reason, Clang 5.0 adds a special symbol ('\01') in front
    // of the mangled names on Windows, making them impossible to find
    // TODO: remove this piece of code and try again when updating Clang
    std::string mncp = mangledName;
    // use corrected symbol for "external" lookup
    if (mncp.size() > 2 && mncp[1] == '?' &&
        mncp.compare(1, 14, std::string("?__cling_Un1Qu"))) {
      mncp.erase(0, 1);
    }
    void *addr = getAddressOfGlobal(mncp, fromJIT);
    if (addr)
      return addr;
    // if failed, proceed with original symbol for lookups in JIT tables
#endif
    return getAddressOfGlobal(mangledName, fromJIT);
  }

  void* Interpreter::getAddressOfGlobal(llvm::StringRef SymName,
                                        bool* fromJIT /*=0*/) const {
    // Return a symbol's address, and whether it was jitted.
    if (isInSyntaxOnlyMode())
      return 0;
    return m_Executor->getAddressOfGlobal(SymName, fromJIT);
  }

  void Interpreter::AddAtExitFunc(void (*Func) (void*), void* Arg) {
    m_Executor->AddAtExitFunc(Func, Arg, getLatestTransaction()->getModule());
  }

  void Interpreter::GenerateAutoloadingMap(llvm::StringRef inFile,
                                           llvm::StringRef outFile,
                                           bool enableMacros,
                                           bool enableLogs) {

    const char *const dummy="cling_fwd_declarator";
    // Create an interpreter without any runtime, producing the fwd decls.
    // FIXME: CIFactory appends extra 3 folders to the llvmdir.
    std::string llvmdir
      = getCI()->getHeaderSearchOpts().ResourceDir + "/../../../";
    cling::Interpreter fwdGen(1, &dummy, llvmdir.c_str(),
                              /*moduleExtensions*/ {}, /*noRuntime=*/true);

    // Copy the same header search options to the new instance.
    Preprocessor& fwdGenPP = fwdGen.getCI()->getPreprocessor();
    HeaderSearchOptions headerOpts = getCI()->getHeaderSearchOpts();
    clang::ApplyHeaderSearchOptions(fwdGenPP.getHeaderSearchInfo(), headerOpts,
                                    fwdGenPP.getLangOpts(),
                                    fwdGenPP.getTargetInfo().getTriple());


    CompilationOptions CO = makeDefaultCompilationOpts();
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = 0;


    std::string includeFile = std::string("#include \"") + inFile.str() + "\"";
    IncrementalParser::ParseResultTransaction PRT
      = fwdGen.m_IncrParser->Compile(includeFile, CO);
    cling::Transaction* T = PRT.getPointer();

    // If this was already #included we will get a T == 0.
    if (PRT.getInt() == IncrementalParser::kFailed || !T)
      return;

    std::error_code EC;
    llvm::raw_fd_ostream out(outFile.data(), EC,
                             llvm::sys::fs::OpenFlags::F_None);
    llvm::raw_fd_ostream log((outFile + ".skipped").str().c_str(),
                             EC, llvm::sys::fs::OpenFlags::F_None);
    log << "Generated for :" << inFile << "\n";
    forwardDeclare(*T, fwdGenPP, fwdGen.getCI()->getSema().getASTContext(),
                   out, enableMacros,
                   &log);
  }

  void Interpreter::forwardDeclare(Transaction& T, Preprocessor& P,
                                   clang::ASTContext& Ctx,
                                   llvm::raw_ostream& out,
                                   bool enableMacros /*=false*/,
                                   llvm::raw_ostream* logs /*=0*/,
                                   IgnoreFilesFunc_t ignoreFiles /*= return always false*/) const {
    llvm::raw_null_ostream null;
    if (!logs)
      logs = &null;

    ForwardDeclPrinter visitor(out, *logs, P, Ctx, T, 0, false, ignoreFiles);
    visitor.printStats();

    // Avoid assertion in the ~IncrementalParser.
    T.setState(Transaction::kCommitted);
  }

  void Interpreter::clearSYCLCompilerNULLTransaction() {
    m_SYCLCompiler->removeCodeByTransaction(NULL);
  }

  void Interpreter::setSYCLCompilerExtractDeclFlag(bool flag) {
    m_SYCLCompiler->setExtractDeclFlag(flag);
  }

  namespace runtime {
    namespace internal {
      Value EvaluateDynamicExpression(Interpreter* interp, DynamicExprInfo* DEI,
                                      clang::DeclContext* DC) {
        Value ret = [&]
        {
          LockCompilationDuringUserCodeExecutionRAII LCDUCER(*interp);
          return interp->Evaluate(DEI->getExpr(), DC,
                                  DEI->isValuePrinterRequested());
        }();
        if (!ret.isValid()) {
          std::string msg = "Error evaluating expression ";
          CompilationException::throwingHandler(nullptr, msg + DEI->getExpr(),
                                                false /*backtrace*/);
        }
        return ret;
      }
    } // namespace internal
  }  // namespace runtime

} //end namespace cling
