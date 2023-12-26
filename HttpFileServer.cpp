/*
* Http file server written in C++20, only supports windows platform.
*/
#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <syncstream>
#include <fstream>
#include <sstream>
#include <string>
#include <ranges>
#include <memory>
#include <exception>
#include <stdexcept>
#include <thread>
#include <queue>
#include <map>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <filesystem>   
#include <source_location>
#include <cstdint>
#include <cctype>

#include <Windows.h>
#include <WinSock2.h>
#include <ws2tcpip.h>

using namespace std::string_literals;
namespace fs = std::filesystem;

static std::string build_response_with_http_code(uint16_t code, const std::string& msg){
    std::string ret;
    std::string html = "<html><h1>"s + msg + "</h1></html>"s;

    ret += "HTTP/1.1 "s + std::to_string(code) + " "s + msg + "\r\n";
    ret += "Content-Type: text/html\r\n";
    ret += "Content-Length: " + std::to_string(html.size()) + "\r\n\r\n";
    ret += html;

    return ret;
}

static const std::string HTTP_200_OK = build_response_with_http_code(200, "OK");
static const std::string HTTP_404_NOT_FOUND = build_response_with_http_code(404, "Not Found");
static const std::string HTTP_405_METHOD_NOT_ALLOWED = build_response_with_http_code(405, "Method Not Allowd");
static const std::string HTTP_414_URI_TOO_LONG = build_response_with_http_code(414, "Uri Too Long");
static const std::string HTTP_500_INTERNAL_SERVER_ERROR = build_response_with_http_code(500, "Internal Server Error");

// constants.
constexpr uint32_t HTTP_RECV_BUFFER_LEN = 8192;
constexpr uint32_t HTTP_RECV_TIMEOUT_SEC = 5;
constexpr uint32_t HTTP_URI_MAX_LEN = 1024;

static std::map<std::string, std::string> HTTP_MIME_TABLE{
    {".css" , "text/css"},
    {".gif" , "image/gif"},
    {".htm" , "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg" , "image/jpeg"},
    {".ico" , "image/x-icon"},
    {".js"  , "application/javascript"},
    {".mp4" , "video/mp4"},
    {".png" , "image/png"},
    {".svg" , "image/svg+xml"},
    {".xml" , "text/xml"}
};

static std::error_code get_last_sys_ec(){
    return std::error_code(GetLastError(), std::system_category());
}

static void print_last_sys_error(const std::string& msg, const std::source_location& slc = std::source_location::current()){
    auto ec = get_last_sys_ec();
    std::osyncstream(std::cerr) << std::format("{}, {}({}): {}, {}\n", slc.file_name(), slc.function_name(), slc.line(), msg, ec.message());
}

static void print_user_error(const std::string& msg, const std::source_location& slc = std::source_location::current()){
    auto ec = get_last_sys_ec();
    std::osyncstream(std::cerr) << std::format("{}, {}({}): {}\n", slc.file_name(), slc.function_name(), slc.line(), msg);
}

static void throw_last_sys_error(const std::string& msg, const std::source_location& slc = std::source_location::current()) {
    auto ec = get_last_sys_ec();
    throw std::runtime_error{ std::format("{}, {}({}): {}, {}", slc.file_name(), slc.function_name(), slc.line(), msg, ec.message()) };
}

static void throw_user_error(const std::string& msg, const std::source_location& slc = std::source_location::current()){
    throw std::runtime_error{ std::format("{}, {}({}): {}", slc.file_name(), slc.function_name(), slc.line(), msg) };
}

static std::wstring conv_ascii_to_unicode(const std::string& str) {
    auto len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, nullptr, 0);
    if (len == 0) {
        throw_last_sys_error("conv_ascii_to_unicode() failed");
    }

    std::wstring buffer(len, wchar_t{});
    len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &buffer[0], len);
    if (len == 0) {
        throw_last_sys_error("conv_ascii_to_unicode() failed");
    }

    /*
    * a very tricky thing here is, if the underlying string stored in fs::path <p> ends with '\0',
    * when you pass the <p> to the fs::directory_iterator, you would get a file not found exception.
    * but if I pass <p.c_str()> to the fs::directory_iterator, then that will work as expected. I tested it on
    * the visual studio and clang++, and both represent this bug, but '\0' doesn't influence the functions like
    * fs::is_directory or fs::is_regular_file, so this is really interesting.
    *
    * This function and the functions below are all doing the encoding conversion,
    * and all of the <len> used in the functions contains '\0', to prevent the tricky behaviour like fs::directory_iterator,
    * we resize the buffer to ignore the tail '\0'.
    */
    buffer.resize(len - 1);
    return buffer;
}

static std::string conv_unicode_to_ascii(const std::wstring& wstr) {
    auto len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len == 0) {
        throw_last_sys_error("conv_unicode_to_ascii() failed");
    }

    std::string buffer(len, char{});

    if (WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &buffer[0], len, nullptr, nullptr) == 0) {
        throw_last_sys_error("conv_unicode_to_ascii() failed");
    }

    buffer.resize(len - 1);   // same reason as std::wstring conv_ascii_to_unicode(const std::string& str);
    return buffer;
}

static std::wstring conv_utf8_to_unicode(const std::string& str) {
    auto len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len == 0) {
        throw_last_sys_error("conv_utf8_to_unicode() failed");
    }

    std::wstring buffer(len, wchar_t{});
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &buffer[0], len - 1);
    if (len == 0) {
        throw_last_sys_error("conv_utf8_to_unicode() failed");
    }

    buffer.resize(len - 1);   // same reason as std::wstring conv_ascii_to_unicode(const std::string& str);
    return buffer;
}

static std::string conv_utf8_to_ascii(const std::string& str) {
    std::wstring wstr = conv_utf8_to_unicode(str);
    return conv_unicode_to_ascii(wstr);
}

static std::string conv_unicode_to_utf8(const std::wstring& wstr) {
    std::string result;

    for (wchar_t c : wstr) {
        auto i = static_cast<uint32_t>(c);   // as you can see, the parameter could also be u32string.

        if (i < 0x80) {
            result += static_cast<char>(i);
        }
        else if (i < 0x800) {
            result += static_cast<char>(0xc0 | (i >> 6));
            result += static_cast<char>(0x80 | (i & 0x3f));
        }
        else if (i < 0x10000) {
            result += static_cast<char>(0xe0 | (i >> 12));
            result += static_cast<char>(0x80 | ((i >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (i & 0x3f));
        }
        else if (i < 0x200000) {
            result += static_cast<char>(0xf0 | (i >> 18));
            result += static_cast<char>(0x80 | ((i >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((i >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (i & 0x3f));
        }
        else {
            result += static_cast<char>(0xf8 | (i >> 24));
            result += static_cast<char>(0x80 | ((i >> 18) & 0x3f));
            result += static_cast<char>(0x80 | ((i >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((i >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (i & 0x3f));
        }
    }

    return result;
}

/*
    Thread pool.
    This thread pool ignores the return value, so you have to push some functions like: void my_func(void);
    Drawing inspiration from Jakob Progsch and yhirose's thread pool implementation.
*/
class ThreadPool {
    bool running;
    std::queue<std::function<void()>> taskQueue;
    std::vector<std::thread> workers;
    std::mutex mut;
    std::condition_variable cv;
public:
    explicit ThreadPool(size_t numOfWorkers)
        : running{ true }
    {
        for (size_t i = 0; i < numOfWorkers; ++i) {
            workers.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock{ mut };
                        cv.wait(lock, [this]() { return !running || !taskQueue.empty(); });

                        if (!running && taskQueue.empty()) {
                            return;
                        }

                        task = std::move(taskQueue.front());
                        taskQueue.pop();
                    }

                    task();
                }
                });
        }
    }

    ThreadPool() : ThreadPool{ std::thread::hardware_concurrency() } {}

    ~ThreadPool() noexcept {
        {
            /* first set running to false. */
            std::unique_lock<std::mutex> lock{ mut };
            running = false;
        }

        cv.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
    }

    void add_task(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock{ mut };
            taskQueue.emplace(std::move(task));
        }

        cv.notify_one();
    }
};

/*
    on windows platform, before you could use socket,
    you have to use WSAStartup() and finally use WSACleanup(),
    so this RAII class is needed.
*/
class WSASetup {
public:
    WSASetup() {
        WSADATA wsaData;

        WORD wVersionRequested = MAKEWORD(2, 2);
        if (WSAStartup(wVersionRequested, &wsaData) != 0) {
            throw_last_sys_error("WSAStartup() failed");
        }
    }

    ~WSASetup() noexcept {
        WSACleanup();
    }
};

static WSASetup wsaSetup;

/*
* http connection, it will handle the http request and response.
*/
class HttpConnection {
    SOCKET sock;
    std::wstring rootPath;
    std::string request;
    std::string method;
    std::string uri;

    bool string_icompare(const std::string& left, const std::string& right){
        return std::ranges::equal(left, right, [](char c1, char c2){
            return std::toupper(c1) == std::toupper(c2);
        });
    }

    int hex_to_decimal(char c) {
        if (c >= '0' && c <= '9'){
            return c - '0';
        }
        else if (c >= 'a' && c <= 'z'){
            return c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'Z'){
            return c - 'A' + 10;
        }

        return -1;
    }

    void uri_decode() {   // uri may contain percent-encoding(like %20), in RFC 3986
        std::string decodeUri;
        auto len = uri.size();

        size_t i = 0;
        while (i < len){
            if (uri[i] == '%' && i + 2 < len){
                decodeUri += static_cast<char>(16 * hex_to_decimal(uri[i + 1]) + hex_to_decimal(uri[i + 2]));
                i += 3;
            }
            else {
                decodeUri += uri[i];
                ++i;
            }
        }

        uri = decodeUri;
    }

    void http_response_send(const std::string& response) {
        send(sock, response.c_str(), static_cast<int>(response.size()), 0);
    }

    void serve_file(const fs::path& p) {
        auto extension = p.extension().string();
        auto iter = HTTP_MIME_TABLE.find(extension);
        std::string contentType;

        if (iter != HTTP_MIME_TABLE.cend()) {
            contentType = iter->second;
        }
        else {
            contentType = "text/plain";
        }

        std::ifstream file(p, std::ios::binary);
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            std::string response = "HTTP/1.1 200 OK\r\nServer: Miku Server\r\nConnection: close\r\n";
            response += "Content-Type: " + contentType + "\r\n";
            response += "Content-Length: " + std::to_string(content.size()) + "\r\n";
            response += "\r\n";

            send(sock, response.c_str(), static_cast<int>(response.size()), 0);
            send(sock, content.c_str(), static_cast<int>(content.size()), 0);
        }
        else {
            http_response_send(HTTP_404_NOT_FOUND);
        }
    }

    std::string build_file_size(uintmax_t size) {   // beautify format.
        if (size < 1024) {
            return std::to_string(size) + " Bytes";
        }
        else if (size >= 1024 && size < 1024 * 1024) {
            return std::to_string(size / 1024) + " KB";
        }
        else if (size >= 1024 * 1024 && size < 1024 * 1024 * 1024) {
            return std::to_string(size / 1024 / 1024) + " MB";
        }
        else {
            return std::to_string(size / 1024 / 1024 / 1024) + " GB";
        }
    }

    void serve_dir(const fs::path& p) {
        std::string response = "HTTP/1.1 200 OK\r\nServer: Miku Server\r\nConnection: close\r\n";
        std::string body = "<html><header><h1>Miku Server</h1></header><body>";
        body += "Current dir: " + conv_unicode_to_utf8(p.wstring()) + "<br><br>";

        for (const auto& entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied)) {
            /*
            * It is necessary to use Unicode to process paths on the Windows platform,
            * while for HTML pages, we use UTF-8.
            */
            std::string name = conv_unicode_to_utf8(entry.path().filename().wstring());

            if (fs::is_directory(entry)) {
                body += "<a href='" + name + "/'>" + name + "/</a><br>";
            }
            else {
                body += "<a href='" + name + "'>" + name + "</a>   " + build_file_size(fs::file_size(entry)) + " <br>";
            }
        }

        body += "</body></html>";
        response += "Content-Type: text/html; charset=utf-8\r\n";
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        response += body;

        send(sock, response.c_str(), static_cast<int>(response.size()), 0);
    }

    void process_request() {
        size_t index = 0;

        // request too large or it is not a valid http request.
        if ((index = request.find("\r\n\r\n")) == std::string::npos) {
            http_response_send(HTTP_500_INTERNAL_SERVER_ERROR);
            return;
        }

        // RFC 2616: parse the first line.
        // 1st space not detected, this is not a valid http request.
        if ((index = request.find(" ")) == std::string::npos) {
            http_response_send(HTTP_500_INTERNAL_SERVER_ERROR);
            return;
        }

        method = request.substr(0, index);
        if (!string_icompare("GET", method)) {
            http_response_send(HTTP_405_METHOD_NOT_ALLOWED);
            return;
        }

        // 2nd space not detected, this is not a valid http request.
        size_t indexBegin = index + 1;
        if ((index = request.find(" ", indexBegin)) == std::string::npos) {
            http_response_send(HTTP_500_INTERNAL_SERVER_ERROR);
            return;
        }

        uri = request.substr(indexBegin, index - indexBegin);
        if (uri.size() > HTTP_URI_MAX_LEN) {   // uri too long.
            http_response_send(HTTP_414_URI_TOO_LONG);
            return;
        }

        fs::path p{ rootPath };
        uri_decode();   // decode the percent-encoding.

        if (uri != "/") {   // if uri is not '/', concatenate the path.
            p /= conv_utf8_to_unicode(uri);   // It is necessary to use Unicode to process paths on the Windows platform.
        }

        std::osyncstream(std::cout) << conv_unicode_to_ascii(p.wstring()) << "\n";

        if (fs::is_directory(p)) {
            serve_dir(p);
        }
        else if (fs::is_regular_file(p)) {
            serve_file(p);
        }
        else {   // not directory or file are considered as not found.
            http_response_send(HTTP_404_NOT_FOUND);
        }
    }
public:
    HttpConnection(SOCKET _sock, const std::string& _rootPath) :
        sock{ _sock },
        rootPath{ conv_ascii_to_unicode(_rootPath) },
        request(HTTP_RECV_BUFFER_LEN, char{})
    {}

    ~HttpConnection() {
        if (sock != INVALID_SOCKET) {
            if (shutdown(sock, SD_SEND) != 0) {   // half close.
                print_last_sys_error("error shutdown()");
            }

            if (closesocket(sock) != 0) {
                print_last_sys_error("error closesocket()");
            }
        }
    }

    void start() {
        // set receive time out.
        uint32_t recvTimeOut = HTTP_RECV_TIMEOUT_SEC * 1000;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeOut), sizeof(recvTimeOut)) != 0) {
            print_last_sys_error("error setsockopt() on SO_RCVTIMEO");
            return;
        }

        auto len = recv(sock, &request[0], HTTP_RECV_BUFFER_LEN, 0);

        if (len < 0) {
            print_last_sys_error("error recv()");
        }
        else if (len == 0) {
            print_user_error("Connection has been closed, nothing would do.\n");
        }
        else {
            process_request();
        }
    }
};

class HttpFileServer {
    SOCKET server;
    ThreadPool pool;

    void bind_listen(const std::string& ip, uint16_t port) {
        struct sockaddr_in addr_in {};

        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(port);   // host byte order to network byte order.
        auto ret = inet_pton(AF_INET, ip.c_str(), &(addr_in.sin_addr));

        if (ret < 0) {
            throw_last_sys_error("error inet_pton()");
        }
        else if (ret == 0) {
            throw_user_error("given ip is not a valid IPv4 dotted-decimal string or a valid IPv6 address string");
        }

        if (bind(server, (const struct sockaddr*)(&addr_in), sizeof(struct sockaddr_in)) != 0) {
            throw_last_sys_error("error bind()");
        }

        if (listen(server, SOMAXCONN) != 0) {
            throw_last_sys_error("error listen()");
        }

        int option = 1;
        if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option), sizeof(option)) != 0) {
            throw_last_sys_error("error setsockopt() on SO_REUSEADDR");
        }
    }
public:
    HttpFileServer() {
        server = socket(AF_INET, SOCK_STREAM, 0);
        if (server == INVALID_SOCKET) {
            throw_last_sys_error("error socket()");
        }
    }

    ~HttpFileServer() {
        if (server != INVALID_SOCKET) {
            if (closesocket(server) != 0) {
                print_last_sys_error("error closesocket()");
            }
        }
    }

    void serve(const std::string& ip, uint16_t port, const std::string& rootPath) {
        bind_listen(ip, port);

        while (true) {
            SOCKET s = accept(server, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                throw_last_sys_error("error accept()");
            }

            auto connection = std::make_shared<HttpConnection>(s, rootPath);
            pool.add_task([connection]() { connection->start(); });
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <root_path>.\n";
        return -1;
    }

    if (!fs::is_directory(argv[2])) {
        std::cerr << "init failed, given root_path: " << argv[2] << " is not a directory, this program won't work on that.";
        return -1;
    }

    try {
        auto port = static_cast<uint16_t>(std::stoi(argv[1]));
        HttpFileServer hfs;

        hfs.serve("0.0.0.0", port, argv[2]);
    }
    catch (const std::invalid_argument& e) {
        std::cerr << e.what() << ", please give a valid port, like 8039, not " << argv[1] << "\n";
    }
    catch (const std::out_of_range& e) {
        std::cerr << e.what() << ", port can't be that big! please give a valid port, like 8039, not " << argv[1] << "\n";
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
    }

    return 0;
}
