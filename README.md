# WinHttpFileServer

##### 一个基于C++20实现的http文件服务器，只支持windows平台，且不依赖任何第三方库.
##### 编写这个程序的初衷是为了向我的朋友们演示如何用C++编写在windows平台工作的网络程序，http文件服务器明显是一个合格的例子。本程序基于同步阻塞式，并提供了一个简易的线程池来处理每一个请求。由于文件服务器必然要展示目录列表，而windows系统的各类unicode，ascii字符编码问题与html默认的utf8结合起来会变得非常晦涩，因此这个程序也展示了对这些问题的处理方式。本程序目前在Visual Studio 2022及clang++上编写并测试。如果你选择在命令行来编译它，首先得确保你有一个支持的C++20的编译器，且编译选项要带上 -l ws2_32，如 clang++ HttpFileServer.cpp -std=c++20 -lws2_32

#####################################################################################################################
##### a http file server implemented in C++20, only supports windows platform and no dependencies on other libraries.
##### The original intention of writing this program was to demonstrate to my friends how to use C++ to write network programs that work on the Windows platform. The HTTP file server is clearly a qualified example. This program is based on synchronous blocking model and provides a simple thread pool to handle each request. Due to the fact that file servers must display directory lists, and the combination of various Unicode and ASCII character encoding issues in Windows systems with the default utf8 in HTML can become very obscure, this program also demonstrates how to handle these issues. This program is currently being written and tested on Visual Studio 2022 and clang++. If you choose to compile it on the command line, you must first ensure that you have a supported C++20 compiler, and the compilation option should include -l ws2_ 32, like: clang++ HttpFileServer.cpp -std=c++20 -lws2_32
