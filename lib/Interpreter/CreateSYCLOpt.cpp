#include "ClingUtils.h"
#include "IncrementalSYCLDeviceCompiler.h"
#include <cling-compiledata.h>

#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/InvocationOptions.h"
#include "cling/Utils/Output.h"
#include "cling/Utils/Paths.h"
#include "cling/Utils/Platform.h"

#include "clang/AST/ASTContext.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Tool.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/VerifyDiagnosticConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/SerializationDiagnostic.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"

#include <cstdio>
#include <ctime>
#include <memory>

using namespace clang;
namespace cling {

class AdditionalArgList {
  typedef std::vector<std::pair<const char *, std::string>> container_t;
  container_t m_Saved;

public:
  void addArgument(const char *arg, std::string value = std::string()) {
    m_Saved.push_back(std::make_pair(arg, std::move(value)));
  }
  container_t::const_iterator begin() const { return m_Saved.begin(); }
  container_t::const_iterator end() const { return m_Saved.end(); }
  bool empty() const { return m_Saved.empty(); }
};

std::string GetExecutablePath(const char *Argv0) {
  void *MainAddr = (void *)intptr_t(GetExecutablePath);
  return llvm::sys::fs::getMainExecutable(Argv0, MainAddr);
}

#ifndef _MSC_VER

static void ReadCompilerIncludePaths(const char *Compiler,
                                     llvm::SmallVectorImpl<char> &Buf,
                                     AdditionalArgList &Args, bool Verbose) {
  std::string CppInclQuery(Compiler);

  CppInclQuery.append(" -xc++ -E -v /dev/null 2>&1 |"
                      " sed -n -e '/^.include/,${' -e '/^ \\/.*++/p' -e '}'");

  if (Verbose)
    cling::log() << "Looking for C++ headers with:\n  " << CppInclQuery << "\n";

  if (FILE *PF = ::popen(CppInclQuery.c_str(), "r")) {
    Buf.resize(Buf.capacity_in_bytes());
    while (fgets(&Buf[0], Buf.capacity_in_bytes(), PF) && Buf[0]) {
      llvm::StringRef Path(&Buf[0]);
      // Skip leading and trailing whitespace
      Path = Path.trim();
      if (!Path.empty()) {
        if (!llvm::sys::fs::is_directory(Path)) {
          if (Verbose)
            cling::utils::LogNonExistantDirectory(Path);
        } else
          Args.addArgument("-cxx-isystem", Path.str());
      }
    }
    ::pclose(PF);
  } else {
    ::perror("popen failure");
    // Don't be overly verbose, we already printed the command
    if (!Verbose)
      cling::errs() << " for '" << CppInclQuery << "'\n";
  }

  // Return the query in Buf on failure
  if (Args.empty()) {
    Buf.resize(0);
    Buf.insert(Buf.begin(), CppInclQuery.begin(), CppInclQuery.end());
  } else if (Verbose) {
    cling::log() << "Found:\n";
    for (const auto &Arg : Args)
      cling::log() << "  " << Arg.second << "\n";
  }
}

static bool AddCxxPaths(llvm::StringRef PathStr, AdditionalArgList &Args,
                        bool Verbose) {
  if (Verbose)
    cling::log() << "Looking for C++ headers in \"" << PathStr << "\"\n";

  llvm::SmallVector<llvm::StringRef, 6> Paths;
  if (!utils::SplitPaths(PathStr, Paths, utils::kFailNonExistant,
                         platform::kEnvDelim, Verbose))
    return false;

  if (Verbose) {
    cling::log() << "Found:\n";
    for (llvm::StringRef Path : Paths)
      cling::log() << " " << Path << "\n";
  }

  for (llvm::StringRef Path : Paths)
    Args.addArgument("-cxx-isystem", Path.str());

  return true;
}

#endif

static std::string getResourceDir(const char *llvmdir) {
  if (!llvmdir) {
    return CompilerInvocation::GetResourcesPath(
        "cling", (void *)intptr_t(GetExecutablePath));
  } else {
    std::string resourcePath;
    llvm::SmallString<512> tmp(llvmdir);
    llvm::sys::path::append(tmp, "lib", "clang", CLANG_VERSION_STRING);
    resourcePath.assign(&tmp[0], tmp.size());
    return resourcePath;
  }
}

static void AddHostArguments(llvm::StringRef clingBin,
                             std::vector<const char *> &args,
                             const char *llvmdir, const CompilerOptions &opts) {
  static AdditionalArgList sArguments;
  if (sArguments.empty()) {
    const bool Verbose = opts.Verbose;
#ifdef _MSC_VER
    std::string VSDir, WinSDK,
        UnivSDK(opts.NoBuiltinInc ? "" : CLING_UCRT_VERSION);
    if (platform::GetVisualStudioDirs(
            VSDir, opts.NoBuiltinInc ? nullptr : &WinSDK,
            opts.NoBuiltinInc ? nullptr : &UnivSDK, Verbose)) {
      if (!opts.NoCXXInc) {
        // The Visual Studio 2017 path is very different than the previous
        // versions (see also GetVisualStudioDirs() in PlatformWin.cpp)
        const std::string VSIncl = VSDir + "\\include";
        if (Verbose)
          cling::log() << "Adding VisualStudio SDK: '" << VSIncl << "'\n";
        sArguments.addArgument("-I", std::move(VSIncl));
      }
      if (!opts.NoBuiltinInc) {
        if (!WinSDK.empty()) {
          WinSDK.append("\\include");
          if (Verbose)
            cling::log() << "Adding Windows SDK: '" << WinSDK << "'\n";
          sArguments.addArgument("-I", std::move(WinSDK));
        } else {
          // Since Visual Studio 2017, this is not valid anymore...
          VSDir.append("\\VC\\PlatformSDK\\Include");
          if (Verbose)
            cling::log() << "Adding Platform SDK: '" << VSDir << "'\n";
          sArguments.addArgument("-I", std::move(VSDir));
        }
      }
    }

#if LLVM_MSC_PREREQ(1900)
    if (!UnivSDK.empty()) {
      if (Verbose)
        cling::log() << "Adding UniversalCRT SDK: '" << UnivSDK << "'\n";
      sArguments.addArgument("-I", std::move(UnivSDK));
    }
#endif

    // Windows headers use '__declspec(dllexport) __cdecl' for most funcs
    // causing a lot of warnings for different redeclarations (eg. coming from
    // the test suite).
    // Do not warn about such cases.
    sArguments.addArgument("-Wno-dll-attribute-on-redeclaration");
    sArguments.addArgument("-Wno-inconsistent-dllimport");

    // Assume Windows.h might be included, and don't spew a ton of warnings
    sArguments.addArgument("-Wno-ignored-attributes");
    sArguments.addArgument("-Wno-nonportable-include-path");
    sArguments.addArgument("-Wno-microsoft-enum-value");
    sArguments.addArgument("-Wno-expansion-to-defined");

    // silent many warnings (mostly during ROOT compilation)
    sArguments.addArgument("-Wno-constant-conversion");
    sArguments.addArgument("-Wno-unknown-escape-sequence");
    sArguments.addArgument("-Wno-microsoft-unqualified-friend");
    sArguments.addArgument("-Wno-deprecated-declarations");

    // sArguments.addArgument("-fno-threadsafe-statics");

    // sArguments.addArgument("-Wno-dllimport-static-field-def");
    // sArguments.addArgument("-Wno-microsoft-template");

#else // _MSC_VER

    // Skip LLVM_CXX execution if -nostdinc++ was provided.
    if (!opts.NoCXXInc) {
      // Need sArguments.empty as a check condition later
      assert(sArguments.empty() && "Arguments not empty");

      SmallString<2048> buffer;

#ifdef _LIBCPP_VERSION
      // Try to use a version of clang that is located next to cling
      // in case cling was built with a new/custom libc++
      std::string clang = llvm::sys::path::parent_path(clingBin);
      buffer.assign(clang);
      llvm::sys::path::append(buffer, "clang");
      clang.assign(&buffer[0], buffer.size());

      if (llvm::sys::fs::is_regular_file(clang)) {
        if (!opts.StdLib) {
#if defined(_LIBCPP_VERSION)
          clang.append(" -stdlib=libc++");
#elif defined(__GLIBCXX__)
          clang.append(" -stdlib=libstdc++");
#endif
        }
        ReadCompilerIncludePaths(clang.c_str(), buffer, sArguments, Verbose);
      }
#endif // _LIBCPP_VERSION

// First try the relative path 'g++'
#ifdef CLING_CXX_RLTV
      if (sArguments.empty())
        ReadCompilerIncludePaths(CLING_CXX_RLTV, buffer, sArguments, Verbose);
#endif
// Then try the include directory cling was built with
#ifdef CLING_CXX_INCL
      if (sArguments.empty())
        AddCxxPaths(CLING_CXX_INCL, sArguments, Verbose);
#endif
// Finally try the absolute path i.e.: '/usr/bin/g++'
#ifdef CLING_CXX_PATH
      if (sArguments.empty())
        ReadCompilerIncludePaths(CLING_CXX_PATH, buffer, sArguments, Verbose);
#endif

      if (sArguments.empty()) {
        // buffer is a copy of the query string that failed
        cling::errs() << "ERROR in cling::CIFactory::createCI(): cannot extract"
                         " standard library include paths!\n";

#if defined(CLING_CXX_PATH) || defined(CLING_CXX_RLTV)
        // Only when ReadCompilerIncludePaths called do we have the command
        // Verbose has already printed the command
        if (!Verbose)
          cling::errs() << "Invoking:\n  " << buffer.c_str() << "\n";

        cling::errs() << "Results was:\n";
        const int ExitCode = system(buffer.c_str());
        cling::errs() << "With exit code " << ExitCode << "\n";
#elif !defined(CLING_CXX_INCL)
        // Technically a valid configuration that just wants to use libClangs
        // internal header detection, but for now give a hint about why.
        cling::errs() << "CLING_CXX_INCL, CLING_CXX_PATH, and CLING_CXX_RLTV"
                         " are undefined, there was probably an error during"
                         " configuration.\n";
#endif
      } else
        sArguments.addArgument("-nostdinc++");
    }

#ifdef CLING_OSX_SYSROOT
    sArguments.addArgument("-isysroot", CLING_OSX_SYSROOT);
#endif

#endif // _MSC_VER

    if (!opts.ResourceDir && !opts.NoBuiltinInc) {
      std::string resourcePath = getResourceDir(llvmdir);

      // FIXME: Handle cases, where the cling is part of a library/framework.
      // There we can't rely on the find executable logic.
      if (!llvm::sys::fs::is_directory(resourcePath)) {
        cling::errs()
            << "ERROR in cling::CIFactory::createCI():\n  resource directory "
            << resourcePath << " not found!\n";
        resourcePath = "";
      } else {
        sArguments.addArgument("-resource-dir", std::move(resourcePath));
      }
    }
  }

  for (auto &arg : sArguments) {
    args.push_back(arg.first);
    args.push_back(arg.second.c_str());
  }
}
static const llvm::opt::ArgStringList *
GetCC1Arguments(clang::driver::Compilation *Compilation,
                clang::DiagnosticsEngine * = nullptr) {
  // We expect to get back exactly one Command job, if we didn't something
  // failed. Extract that job from the Compilation.
  const clang::driver::JobList &Jobs = Compilation->getJobs();
  if (!Jobs.size() || !isa<clang::driver::Command>(*Jobs.begin())) {
    // diagnose this better...
    cling::errs() << "No Command jobs were built.\n";
    return nullptr;
  }

  // The one job we find should be to invoke clang again.
  const clang::driver::Command *Cmd =
      cast<clang::driver::Command>(&(*Jobs.begin()));
  if (llvm::StringRef(Cmd->getCreator().getName()) != "clang") {
    // diagnose this better...
    cling::errs() << "Clang wasn't the first job.\n";
    return nullptr;
  }

  return &Cmd->getArguments();
}
void IncrementalCompileOpt(Interpreter *interp, std::vector<const char *> &m_Args) {
  std::string cppStdVersion;
  clang::LangOptions langOpts = interp->getCI()->getLangOpts();
  cling::CompilerOptions COpts = interp->getOptions().CompilerOpts;
  // Set the c++ standard. Just one condition is possible.
  if (langOpts.CPlusPlus11)
    cppStdVersion = "-std=c++11";
  if (langOpts.CPlusPlus14)
    cppStdVersion = "-std=c++14";
  if (langOpts.CPlusPlus1z)
    cppStdVersion = "-std=c++1z";
  if (langOpts.CPlusPlus2a)
    cppStdVersion = "-std=c++2a";

  const size_t argc = COpts.Remaining.size();
  const char *const *argv = &COpts.Remaining[0];
  std::vector<const char *> argvCompile(argv, argv + 1);
  argvCompile.reserve(argc + 5);
  std::string overlayArg;
  std::string cacheArg;
  if (COpts.CxxModules) {
    argvCompile.push_back("-fmodules");
    argvCompile.push_back("-fcxx-modules");
    argvCompile.push_back("-Xclang");
    argvCompile.push_back("-fmodules-local-submodule-visibility");
    if (!COpts.CachePath.empty()) {
      cacheArg = std::string("-fmodules-cache-path=") + COpts.CachePath;
      argvCompile.push_back(cacheArg.c_str());
    }
    argvCompile.push_back("-Xclang");
    argvCompile.push_back("-fdisable-module-hash");
    argvCompile.push_back("-Wno-module-import-in-extern-c");
    argvCompile.push_back("-Wno-modules-import-nested-redundant");
    argvCompile.push_back("-Xclang");
    argvCompile.push_back("-fno-validate-pch");
  }

  if (!COpts.Language) {
    // We do C++ by default; append right after argv[0] if no "-x" given
    argvCompile.push_back("-x");
    argvCompile.push_back("c++");
  }
  argvCompile.push_back(cppStdVersion.c_str());
  argvCompile.insert(argvCompile.end(), argv + 1, argv + argc);
  std::string ClingBin = GetExecutablePath(argv[0]);
  AddHostArguments(ClingBin, argvCompile, NULL, COpts);

#ifdef __APPLE__
  if (!COpts.StdLib) {
#ifdef _LIBCPP_VERSION
    argvCompile.push_back("-stdlib=libc++");
#elif defined(__GLIBCXX__)
    argvCompile.push_back("-stdlib=libstdc++");
#endif
  }
#endif

  if (!COpts.HasOutput) {
    argvCompile.push_back("-c");
    argvCompile.push_back("-");
  }

  auto InvocationPtr = std::make_shared<clang::CompilerInvocation>();

  // The compiler invocation is the owner of the diagnostic options.
  // Everything else points to them.
  DiagnosticOptions &DiagOpts = InvocationPtr->getDiagnosticOpts();
  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagIDs(new DiagnosticIDs());

  std::unique_ptr<TextDiagnosticPrinter> DiagnosticPrinter(
      new TextDiagnosticPrinter(cling::errs(), &DiagOpts));

  llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags(new DiagnosticsEngine(
      DiagIDs, &DiagOpts, DiagnosticPrinter.get(), /*Owns it*/ true));
  DiagnosticPrinter.release();

  if (!Diags) {
    cling::errs() << "Could not setup diagnostic engine.\n";
  }

  llvm::Triple TheTriple(llvm::sys::getProcessTriple());
#ifdef LLVM_ON_WIN32
  // COFF format currently needs a few changes in LLVM to function properly.
  TheTriple.setObjectFormat(llvm::Triple::COFF);
#endif

  clang::driver::Driver Drvr(argv[0], TheTriple.getTriple(), *Diags);
  // Drvr.setWarnMissingInput(false);
  Drvr.setCheckInputsExist(false); // think foo.C(12)
  llvm::ArrayRef<const char *> RF(&(argvCompile[0]), argvCompile.size());
  std::unique_ptr<clang::driver::Compilation> Compilation(
      Drvr.BuildCompilation(RF));

  if (!Compilation) {
    cling::errs() << "Couldn't create clang::driver::Compilation.\n";
  }

  const driver::ArgStringList *CC1Args = GetCC1Arguments(Compilation.get());
  if (!CC1Args) {
    cling::errs() << "Could not get cc1 arguments.\n";
  }
  auto it = CC1Args->begin();
  bool findValidArg = false;
  while (true) {
    if (it == CC1Args->end())
      break;
    std::string arg(*it);
    if (arg[0] == '-') {
      findValidArg = false;
    }
    if (findValidArg || arg == "-cxx-isystem" || arg == "-internal-isystem" ||
        arg == "-internal-externc-isystem" || arg.substr(0, 8) == "-std=c++" ||
        arg.substr(0, 2) == "-f" || arg == "-x") {
      char *argCString = new char[arg.size() + 1];
      strcpy(argCString, arg.c_str());
      m_Args.push_back(argCString);
      findValidArg = true;
    }
    it++;
  }
  char *noWarnings = new char[3];
  char *targetFile =
      new char[IncrementalSYCLDeviceCompiler::dumpFile.length() + 1];
  strcpy(noWarnings, "-w");
  strcpy(targetFile, IncrementalSYCLDeviceCompiler::dumpFile.c_str());
  m_Args.push_back(noWarnings);
  m_Args.push_back(targetFile);
}
} // namespace cling