ALCaMI: Append Log Cache Manager Interface
===========================================

A C++20 framework for concurrent caching with user-defined eviction policies.

> [!IMPORTANT]
> ALCaMI is a work in progress and is still unstable.

[[_TOC_]]

Build Requirements
------------------

> [!NOTE]
> Build dependencies are subject to change.

If you want to build ALCaMI from source, make sure you have all the software listed below.

### Operating Systems

While we try to stay platform-agnostic, we only guarantee that ALCaMI works on Linux. With that said, ALCaMI currently works on macOS and we expect that ALCaMI will work on other UNIXes. We have no plans to support Windows.

### Compilers

ALCaMI is a C++20 library and requires a compiler that supports most of the C++20 standard. In particular, we try to stay compatible with the following compiler versions.

| **Dependency** | Lowest Tested Version |
|----------------|-----------------------|
| GCC            | 14.2.0                |
| Clang          | 16.0.6                |

### Tools

You will need the following tools to build ALCaMI.

| **Dependency** | Lowest Tested Version |
|----------------|-----------------------|
| CMake          | 3.31.6                |
| Make           | --                    |
| Git            | 2.47.3                |

We try to be independent of the generator, but we only guarantee compatibility with Make.

### Libraries

ALCaMI requires the following libraries.

| **Dependency** | Lowest Tested Version   |
|----------------|-------------------------|
| Boost          | 1.83.0                  |
| fmt            | 10.1.1                  |
| spdlog         | 1.15.2                  |
| TBB            | --                      |


ALCaMI also requires [Microsoft's implementation](https://github.com/microsoft/GSL) of the [GSL](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#gsl-guidelines-support-library), which can be installed along-side ALCaMI by configuring CMake with `-DALCAMI_INSTALL_MSGSL=1`.

Installing
----------

The main way to install ALCaMI is by building form source.

### Step-by-Step

Run the following commands from the project root.

```sh
$ scripts/configure release
$ scripts/build
$ scripts/install
```

This will install ALCaMI in `build/install`.

To change the install directory, use the `-DCMAKE_INSTALL_PREFIX` when configuring the build system. E.g., `scripts/configure release -DCMAKE_INSTALL_PREFIX=/path/to/install && scripts/build && scripts/install`.

Getting Started
---------------

> [!NOTE]
> This section is a work in progress.

Examples of using ALCaMI can be found in the `examples/` directory.

Documentation
-------------

### Online Documentation

An online version of documentation is available [here](https://score-group.gitlab.io/alcami).

### Generating Documentation

ALCaMI uses Doxygen for documentation. To generate the documentation, configure CMake with the `-DALCAMI_BUILD_DOXYGEN` option, then run the `scripts/doxygen` script from the repository root. This will put the documentation in `build/doxygen/html/`.

Contributing
------------

Please see [CONTRIBUTING.md](/CONTRIBUTING.md).

License and Copyright
---------------------

:copyright: [2026 Andrew J. Mikalsen](NOTICE). ALCaMI is distributed under the terms of the [Apache License, Version 2.0](LICENSE).

How to Cite
-----------

If you found ALCaMI useful, please cite this repository.
