# The Quarzum Programming language

Welcome to Quarzum, a compiled general programming language.

## Language features

* Full imperative, object-oriented and functional paradigms supported
* Enough concrete for low-level, enough abstraction for high-level
* Easy sintax and low learning curve
* Strong, static type system
* Built-in memory allocation keywords
* Memory ownership and security in compile-time

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
quarzum my-file.qz
```

## Compiling the project

If you don't have Quarzum installed, you must install it from the **latest release**.
Then, in a terminal, execute the following command:

```sh
quarzum version # Ensure Quarzum is installed
mkdir build
quarzum src/quarzum.qz --build -o build/quarzum
```

## Creating a library

Wanting to create a custom library? Just copy your files in a folder, inside the compiler library folder.

* For Linux, it should be `/usr/lib/quarzum`
* For Windows, it should be `C:\Programs (x86)\quarzum\lib`

Now you can make reference to your new library like this example:

```qz
import "@my-library/my-file.qz"
```