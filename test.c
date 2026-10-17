#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
typedef struct
{
    int keyCode;
    int active;
} KeyPoint;

int tcp_connect(char *ip, int port)
{
    int client_socket;
    struct sockaddr_in server_addr;
    int ret;

    // 设置服务器地址和端口

    // 创建Socket
    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1)
    {
        perror("Socket创建失败");
        exit(EXIT_FAILURE);
    }
    printf("客户端Socket创建成功\n");

    // 配置服务器地址信息
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    // 连接到服务器
    ret = connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret == -1)
    {
        perror("连接服务器失败");
        close(client_socket);
        exit(EXIT_FAILURE);
    }
    printf("成功连接到服务器 %s:%d\n", ip, port);
    return client_socket;
}

int main()
{
    int tcpfd = tcp_connect("192.168.100.2", 9000);
    KeyPoint keypoint;
    int ret = 0;
    int i = 1;
    char buf[255];
    while (1)
    {
        read(0, buf, 255);
        if (buf[0] == 'q')
            break;
        keypoint.keyCode = 33;
        keypoint.active = i;
        ret = sizeof(keypoint);
        write(tcpfd, &ret, 4);
        write(tcpfd, &keypoint, sizeof(keypoint));
        i = !i;

    }
}