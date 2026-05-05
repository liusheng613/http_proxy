#include "proxy.h"

int main()
{
    // 监听 8080, 转发到 127.0.0.1:9000
    TcpProxy proxy(8080, "127.0.0.1", 9000);
    proxy.run();
    return 0;
}