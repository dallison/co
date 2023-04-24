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
  urls = ["https://github.com/dallison/cocpp/archive/refs/tags/1.0.0.tar.gz"],
  strip_prefix = "cocpp-1.0.0",
  sha256 = "c3c088e39aba59af0f579af697c0ee2baa51befe029ca092f05455411ec42191"
)
```

Please take a look at the C version's README.md for details on how to use it.


