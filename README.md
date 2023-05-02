# cocpp
Coroutine library in C++

This is a C++ version of my [C coroutines library](https://github.com/dallison/coroutines).
It has a similar API and implementation but the following differences are evident:

1. Has a C++ class interface rather than C structs and access functions
1. Memory management has been removed and is delegated to the application.
1. Supports std::function as the coroutine execution function and thus allows
the use of lambdas as well as regular functions.

It is built using [Google's Bazel](https://github.com/bazelbuild/bazel).  The following
WORKSPACE entry can be used to import it into another Bazel-built system:

```
http_archive(
  name = "coroutines",
  urls = ["https://github.com/dallison/cocpp/archive/refs/tags/1.1.0.tar.gz"],
  strip_prefix = "cocpp-1.1.0",
  sha256 = "42cb1aafe7724b6c9cd1d42d8e9fb0ae518d4baefc8e39c762e622b736196c71"
)
```

Please take a look at the C version's README.md for details on how to use it.


