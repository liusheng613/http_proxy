#include "proxy.h"
#include "proxy_epoll.h"

int main(int argc, char** argv)
{
    // 监听 8080, 转发到 127.0.0.1:1024
    if (argc == 2)
    {
        TcpProxy proxy(8080, "127.0.0.1", 1024);
        proxy.run();
    }
    else
    {
        TcpProxy2 proxy(8080, "127.0.0.1", 1024);
        proxy.run();
    }

    return 0;
}
