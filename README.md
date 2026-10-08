# The Quarzum Programming language

Welcome to Quarzum, a compiled general programming language.

## Language features

* Full imperative, object-oriented and functional paradigms supported
* Enough concrete for low-level, enough abstraction for high-level
* Easy sintax and low learning curve
* Strong, static type system
* Unified `struct` types: instantiate by value with `Name(...)` or on the heap
  with `new Name(...)`, which returns a `ptr<Name>`
* Interfaces via `trait` + `implements` (no inheritance)
* Built-in memory allocation keywords (`new` / `free`)
* Memory ownership and security in compile-time
* C interoperability: call libc and other native libraries via `extern function`

## How to run a Quarzum file

Create a Quarzum (.qz) file. Let's create a Hello World program:

```qz
import "@std/io.qz"

function main(){
    println("Hello world!");
}
```

With Quarzum installed, execute this command in a terminal:

```sh
quarzumc my-file.qz
```

## Compiling the project

To compile the project, you can use `make` on the root folder of the project. Or else, you can use `scripts/run.sh` to also delete temporal files.

## Creating a library

Wanting to create a custom library? Just copy your files in a folder, inside the compiler library folder.

* For Linux, it should be `/usr/lib/quarzum`
* For Windows, it should be `C:\Programs (x86)\quarzum\lib`

Now you can make reference to your new library like this example:

```qz
import "@my-library/my-file.qz"
```