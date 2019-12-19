Cling with SYCL
=========================================

Cling C++ Interpreter with Intel SYCL extension. The root repository is at [https://github.com/root-project/cling](https://github.com/root-project/cling)

Installation
------------
### Download SYCL compiler with jupyter-patch
Clone and build SYCL Compiler following [this guide](https://gitlab.devtools.intel.com/shqa/llvm/blob/jupyter-patch/sycl/doc/GetStartedWithSYCLCompiler.md)
### Download llvm and clang with cling-patches
Create a root directory for this project:
```bash
mkdir cling && cd cling
```
Then clone llvm and clang with cling-patches:
```bash
git clone https://gitlab.devtools.intel.com/shqa/root-llvm-mirror.git src
cd src
git checkout cling-patches
cd tools
<<<<<<< HEAD
<<<<<<< HEAD
git clone https://gitlab.devtools.intel.com/shqa/root-clang-mirror.git clang
=======
git clone https://gitlab.devtools.intel.com/shqa/root-clang-mirror.git
>>>>>>> 475805a7... mirror llvm and clang from root to internal gitlab
=======
git clone https://gitlab.devtools.intel.com/shqa/root-clang-mirror.git clang
>>>>>>> d74c8c6c... clear DeclPrint and correct README
cd clang
git checkout cling-patches
```
### Download cling with SYCL extension
You are now in the clang repository, then run:
```bash
cd ..
git clone https://gitlab.devtools.intel.com/shqa/jupyter-notebook-with-sycl.git cling
```
### Build cling
Return to the root directory and use cmake to configure the project:
```bash
cd ../..
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=. -DCMAKE_BUILD_TYPE=[Release or Debug] ../src
```
Then build the project:
```bash
make -j`nproc`
```

Environment Settings
------------
### Set library path
Add the path that contains `libsycl.so` to your `$LD_LIBRARY_PATH` environment variable. For instance:
```bash
export LD_LIBRARY_PATH=$SYCL_HOME/build/lib:$LD_LIBRARY_PATH
```
### Set include path
Add OpenCL headers and sycl headers to your `$CPLUS_INCLUDE_PATH` environment variable. For instance:
```bash
export CPLUS_INCLUDE_PATH=OpenCL-Headers:$SYCL_HOME/llvm/sycl/include:$CPLUS_INCLUDE_PATH
```
### Set SYCL compiler executable path
Set the SYCL compiler bin path to `$SYCL_BIN_PATH` environment variable. For instance:
```bash
export SYCL_BIN_PATH=$SYCL_HOME/build/bin
```
### Set SYCL compiler executable path
Set the SYCL compiler bin path to `$SYCL_BIN_PATH` environment variable. For instance:
```bash
export SYCL_BIN_PATH=$SYCL_HOME/build/bin
```

Usage
------------
Lauch cling with -fsycl argument:
```bash
cling -fsycl
```

Jupyter
------------
Cling comes with a [Jupyter](http://jupyter.org) kernel. After building cling,
install Jupyter and cling's kernel by following the README.md in
[tools/Jupyter](tools/Jupyter). Make sure cling is in your PATH when you start jupyter!