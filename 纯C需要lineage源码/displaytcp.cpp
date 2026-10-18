#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>

#define TAG "scrcpy_c_networking"

#include <android/native_window.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h> // 必须引入此头文件以支持 Surface 转换
#include <ui/Rect.h>
#include <ui/DisplayInfo.h>
#include <binder/ProcessState.h>

static android::sp<android::IBinder> g_dpy = nullptr;

extern "C" int native_bind_display_to_surface(ANativeWindow *window, int width, int height)
{
    using namespace android;

    if (window == nullptr){
        printf("window == nullptr\n");
        return -1;
    }

    // 1. Android 9 创建虚拟显示器
    sp<IBinder> dpy = SurfaceComposerClient::createDisplay(String8("my_display"), false);
    if (dpy == nullptr)
        return -1;

    // 2. 修复：使用 static_cast 替代 reinterpret_cast 进行安全的向下转型
    sp<Surface> surface = static_cast<Surface *>(window);
    if (surface == nullptr)
        return -1;

    sp<IGraphicBufferProducer> producer = surface->getIGraphicBufferProducer();
    if (producer == nullptr)
        return -1;

    // 3. Android 9 使用 Transaction 替代全局静态事务
    SurfaceComposerClient::Transaction t;

    t.setDisplaySurface(dpy, producer);

    Rect layerStackRect(0, 0, width, height);
    Rect displayRect(0, 0, width, height);

    t.setDisplayProjection(dpy, static_cast<ui::Rotation>(0), layerStackRect, displayRect);
    t.setDisplayLayerStack(dpy, 0);
    // 4. 提交事务
    t.apply();

    // 保存 dpy 供 unbind 使用
    g_dpy = dpy;
    return 0;
}

extern "C" void native_unbind_display(void)
{
    using namespace android;

    if (g_dpy != nullptr)
    {
        SurfaceComposerClient::Transaction t;
        t.setDisplaySurface(g_dpy, nullptr);
        t.apply();

        SurfaceComposerClient::destroyDisplay(g_dpy);
        g_dpy = nullptr;
        printf("取消绑定\n");
    }
}
int sendd(int client_fd, char *buf, int ssize)
{
    //  printf("sendd %d\n", ssize);
    int ret = 0;
    int size = 0;
    ret = ssize - size;
    while (ret > 0)
    {
        ret = write(client_fd, buf, ssize - size);
        if (ret < 1)
        {
            return -1;
        }
        buf += ret;
        size += ret;
        ret = ssize - size;
    }
    return size;
}
void settcp(int client_fd)
{
    // 1. 设置发送超时 (例如 1 秒)
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)); // 设置接收超时 (例如 1 秒)
}
int client_connect(int server_fd)
{
    struct sockaddr_in6 client_addr;
    int addrlen = sizeof(struct sockaddr_in6);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&addrlen);
    if (client_fd < 0)
    {
        printf("客户端连接失败\n");
        return -1;
    }
    int is_v6_loopback = (IN6_IS_ADDR_LOOPBACK(&client_addr.sin6_addr));

    // 情况 B：IPv4 映射到 IPv6 的本地回环地址，即 "::ffff:127.0.0.1"
    int is_v4_mapped_loopback = (IN6_IS_ADDR_V4MAPPED(&client_addr.sin6_addr) &&
                                 client_addr.sin6_addr.s6_addr[12] == 127 &&
                                 client_addr.sin6_addr.s6_addr[13] == 0 &&
                                 client_addr.sin6_addr.s6_addr[14] == 0 &&
                                 client_addr.sin6_addr.s6_addr[15] == 1);

    if (is_v6_loopback || is_v4_mapped_loopback)
    {
        printf("⚠️ 拦截到本地回环地址连接（IPv4/IPv6），已自动断开。\n");
        close(client_fd);
        return -1;
    }
    settcp(client_fd);
    return client_fd;
}

int main(int argc, char **argv)
{
    // 忽略 SIGPIPE 信号，防止客户端断开时程序闪退
    signal(SIGPIPE, SIG_IGN);
    char buffer[255];
    int client_max = 10;
    int client_count = 0;
    int bitrate = 8000000; // 8M
    int port = 9999;
    if (argc > 1 && strstr(argv[1], "-h"))
    {
        printf("Usage: %s [port] [bitrate] [width] [height]\n", argv[0]);
        return 0;
    }
    if (argc > 1)
    {
        port = atoi(argv[1]);
    }
    if (argc > 2)
    {
        bitrate = atoi(argv[2]);
    }
    int *Width = (int *)buffer;
    *Width = 1920;
    int *Height = (int *)(buffer + 4);
    *Height = 1080;
    if (argc > 4)
    {
        *Width = atoi(argv[3]);
        *Height = atoi(argv[4]);
    }
    printf(" ==============%dX%d  port: %d bitrate: %d============ \n", *Width, *Height, port, bitrate);
    int *sps_size = (int *)(buffer + 8);
    char *sps_buf = buffer + 12;
    int *client_arr = (int *)malloc(sizeof(int) * client_max);
    ANativeWindow *window = nullptr;
    struct sockaddr_in6 server_addr; //, client_addr;
    int addrlen = sizeof(struct sockaddr_in6);
    // printf("开始监听端口 %d\n", port);
    int server_fd = socket(AF_INET6, SOCK_STREAM, 0); // 创建套接字
    if (server_fd < 0)
    {
        perror("socket");
        return -1;
    }
    memset(&server_addr, 0, addrlen);
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any; // 绑定到所有可用的网络接口
    server_addr.sin6_port = htons(port); // 绑定到指定的端口
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 设置 SO_REUSEADDR 选项
    // ⚠️ 关键修改 3：关闭 IPV6_V6ONLY 选项（允许该 IPv6 Socket 接收 IPv4 连接）
    int v6only = 0;
    if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
    {
        perror("关闭 IPV6_V6ONLY 失败");
        close(server_fd);
        return 1;
    }
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, 1) < 0)
    {
        perror("listen");
        close(server_fd);
        return -1;
    }
    int runing = 1;
    AMediaCodec *codec = AMediaCodec_createEncoderByType("video/avc");
    if (!codec)
    {
        printf("AMediaCodec_createEncoderByType failed\n");
        close(server_fd);
        return -1;
    }

    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, *Width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, *Height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 60); // 60fps
    // max-fps
    //  AMediaFormat_setFloat(format, "max-fps-to-encoder", 60.0f);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);      // 关键：I 帧间隔为 1 秒
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 2130708361); // COLOR_FormatSurface
    AMediaFormat_setInt32(format, "profile", 8);                              // H.264 Baseline Profile
    AMediaFormat_setInt32(format, "level", 65536);                            // H.264 Level 3.1
    media_status_t status = AMediaCodec_configure(codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK)
    {
        AMediaCodec_delete(codec);
        fprintf(stderr, "AMediaCodec_configure 失败\n");
        close(server_fd);
        return 0;
    }
    AMediaFormat_delete(format);

    // 3. 【核心修复】：直接让 AMediaCodec 在 C 层创建 Input Surface (ANativeWindow)
    // 这样创建出来的 Window 内部天生携带着完美的、由多媒体框架初始化的 BufferQueue
    status = AMediaCodec_createInputSurface(codec, &window);
    if (status != AMEDIA_OK || !window)
    {
        fprintf(stderr, "AMediaCodec_createInputSurface 失败\n");
        AMediaCodec_delete(codec);
        close(server_fd);
        return -1;
    }

    // 4. 启动编码器
    status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK)
    {
        ANativeWindow_release(window);
        AMediaCodec_delete(codec);
        fprintf(stderr, "AMediaCodec_start 失败\n");
        close(server_fd);
        return -1;
    }
    int client_fd = 0;
    int no_client_fd = 0;
    fd_set wd_set, rd_set;
    int maxfd = 0;
    ssize_t buf_idx = 0;
    uint8_t *buf = 0;
    uint8_t *nalu_data = 0;
    size_t outx = 0;
    int out_size; // 4bit
    int ret = 0;
    *sps_size = 0;
    AMediaCodecBufferInfo info;
    while (runing)
    {
        if (client_count == 0)
        {
           
            if (g_dpy != nullptr)
                native_unbind_display();
            client_fd = client_connect(server_fd);
            if (client_fd < 0)
            {
                continue;
            }
            out_size = 8; // 8bit
            ret = sendd(client_fd, buffer, out_size);
            if (ret != out_size)
            {
                fprintf(stderr, "TCP %d 客户端发送失败。\n", client_fd);
                close(client_fd);
                continue;
            }
            if (g_dpy == nullptr)
            {
                printf("绑定显示\n");
                ret = native_bind_display_to_surface(window, *Width, *Height);
                if (ret < 0)
                {
                    fprintf(stderr, "native_bind_display_to_surface 失败\n");
                    close(client_fd);
                    continue;
                }
            }
            client_arr[client_count++] = client_fd;
            printf("客户端连接成功 fd=%d count=%d\n", client_fd, client_count);
            no_client_fd = client_fd;
            client_fd = 0;
        }
        buf_idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 20000);
        if (buf_idx >= 0)
        {
            buf = AMediaCodec_getOutputBuffer(codec, buf_idx, &outx);
            if (buf && info.size > 0)
            {
                FD_ZERO(&wd_set);
                FD_ZERO(&rd_set);
                FD_SET(server_fd, &rd_set);
                maxfd = server_fd;
                for (int i = 0; i < client_count; ++i)
                {
                    FD_SET(client_arr[i], &wd_set);
                    if (client_arr[i] > maxfd)
                    {
                        maxfd = client_arr[i];
                    }
                }
                ret = select(maxfd + 1, &rd_set, &wd_set, 0, 0);
                if (ret < 0)
                {
                    perror("错误 select()");
                    break;
                }
                nalu_data = buf + info.offset;
                //  out_size = info.size;
                if ((nalu_data[4] & 0x1f) == 5)
                {
                    if (no_client_fd)
                    {
                        client_fd = no_client_fd;
                        no_client_fd = 0;
                    }
                    // printf("是I帧 %#x\n",nalu_data[4]);
                    if (FD_ISSET(server_fd, &rd_set))
                    {
                       // printf("有新客户端连接\n");
                        client_fd = client_connect(server_fd);
                        if (client_fd < 0)
                        {
                            continue;
                        }

                        out_size = 8;
                        ret = sendd(client_fd, buffer, out_size);
                        if (ret != out_size)
                        {
                            fprintf(stderr, "TCP %d 客户端发送失败。\n", client_fd);
                            close(client_fd);
                            continue;
                        }
                        client_arr[client_count++] = client_fd;
                        if (client_count == client_max)
                        {
                            client_max *= 2;
                            client_arr = (int *)realloc(client_arr, sizeof(int) * client_max);
                            if (!client_arr)
                                break;
                        }
                        printf("客户端连接成功 fd=%d count=%d\n", client_fd, client_count);
                    }
                }
                for (int i = 0; i < client_count; i++)
                {

                    if (FD_ISSET(client_arr[i], &wd_set))
                    {
                        if (no_client_fd == client_arr[i])
                            continue;
                        out_size = info.size;
                        if (client_fd == client_arr[i])
                        {
                            out_size += *sps_size;
                        }
                        ret = sendd(client_arr[i], (char *)&out_size, sizeof(int));
                        if (ret != sizeof(int))
                        {
                            fprintf(stderr, "TCP %d 客户端似乎断开了连接，发送失败还有%d。\n", client_arr[i], client_count - 1);
                            close(client_arr[i]);
                            memcpy(client_arr + i, client_arr + i + 1, sizeof(int) * (client_count - i - 1));
                            client_count--;
                            i--;
                            continue;
                        }
                        if (client_fd == client_arr[i])
                        {
                            printf("发送SPS PPS\n");
                            ret = sendd(client_arr[i], sps_buf, *sps_size);
                            if (ret != *sps_size)
                            {
                                fprintf(stderr, "TCP %d 客户端似乎断开了连接，发送失败还有%d。\n", client_arr[i], client_count - 1);
                                close(client_arr[i]);
                                memcpy(client_arr + i, client_arr + i + 1, sizeof(int) * (client_count - i - 1));
                                client_count--;
                                i--;
                                continue;
                            }
                            client_fd = 0;
                        }
                        ret = sendd(client_arr[i], (char *)nalu_data, info.size);
                        if (ret != info.size)
                        {
                            fprintf(stderr, "TCP %d 客户端似乎断开了连接，发送失败还有%d。\n", client_arr[i], client_count - 1);
                            close(client_arr[i]);
                            memcpy(client_arr + i, client_arr + i + 1, sizeof(int) * (client_count - i - 1));
                            client_count--;
                            i--;
                            continue;
                        }
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, buf_idx, false);
        }
        if (buf_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            out_size = 0;
            AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
            // printf("C++ 捕获编码格式改变: %s\n", AMediaFormat_toString(format));
            //  【核心修复】：提取 H.264 的首要配置帧（SPS 和 PPS）

            buf = (uint8_t *)sps_buf;
            char *p = 0;
            // 从 format 中提取 csd-0 (SPS) 和 csd-1 (PPS)
            if (AMediaFormat_getBuffer(format, "csd-0", (void **)&p, &outx) && p)
            {
                memcpy((void *)buf, (void *)p, outx);
                buf += outx;
                *sps_size = 0;
                *sps_size += outx;
                printf("成功保存  SPS 配置帧，大小: %zu", outx);
            }

            if (AMediaFormat_getBuffer(format, "csd-1", (void **)&p, &outx) && p)
            {
                memcpy(buf, p, outx);
                *sps_size += outx;
                printf("  成功保存 PPS 配置帧，大小: %zu  总大小%d\n", outx, *sps_size);
            }
            AMediaFormat_delete(format);
        }
    }
    for (int i = 0; i < client_count; i++)
    {
        close(client_arr[i]);
    }
    if (codec)
    {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (window)
        ANativeWindow_release(window);
    close(server_fd);
}
