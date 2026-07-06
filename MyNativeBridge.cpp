#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>

#define TAG "scrcpy_c_networking"
// #define printf(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
// #define fprintf(stderr,...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define MAX_CLIENT_COUNT 666

int *client_arr = 0;
int client_count = 0;
char *start_buf = 0;
int Width = 0;
int Height = 0;
struct CodecContext
{
    AMediaCodec *codec = nullptr;
    ANativeWindow *window = nullptr;
    //  pthread_t thread;
    bool is_running = false;
    int server_fd = -1;
    // int client_fd = -1;
} g_ctx;
/*
// 暂停编码：通知编码器丢弃后续输入帧
AMediaFormat* params = AMediaFormat_new();
AMediaFormat_setInt32(params, "drop-input-frames", 1); // 1 代表挂起 (Suspend)
AMediaCodec_setParameters(codec, params);
AMediaFormat_delete(params);

// 恢复编码：通知编码器重新接收输入帧
AMediaFormat* params = AMediaFormat_new();
AMediaFormat_setInt32(params, "drop-input-frames", 0); // 0 代表恢复 (Resume)
AMediaCodec_setParameters(codec, params);
AMediaFormat_delete(params);
*/
int sendd(int client_fd, char *buf, int ssize, int flags)
{
    int ret = 0;
    int size = 0;
    ret = ssize - size;
    while (ret > 0)
    {
        ret = write(client_fd, buf, ssize - size);
        if (ret < 0)
        {
            return -1;
        }
        buf += ret;
        size += ret;
        ret = ssize - size;
    }
    return size;
}

void *client_thread(void *arg)
{
    int clientfd[MAX_CLIENT_COUNT];
    client_arr = clientfd;
    int server_fd = *(int *)arg;
    printf("启动客户端线程 %d\n", server_fd);
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int client_fd = 0;
    int ret = 0;
    char buf[255];
    int *py=0;
    
    while (1)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        py=(int *)start_buf;
        printf("====%dx%d size: %d \n",py[0],py[1],py[2]);
        if (client_fd < 0)
        {
            printf("客户端连接失败\n");
            continue;
        }
        //ret=read(client_fd, buf, 255);
        ret = sendd(client_fd, start_buf, py[2]+4+4+4, 0);
        if (ret != py[2]+4+4+4)
        {
            printf("发送失败\n");
            close(client_fd);
            continue;
        }
        client_arr[client_count++] = client_fd;
        printf("客户端连接成功 %d count=%d\n", client_fd, client_count);
    }

    return nullptr;
}
// C++ 层的核心循环：不断从硬编码器提取 H.264，并通过 C++ 建立的 TCP Socket 直接发送
void *codec_output_thread(void *arg)
{
    char start[1255];
    CodecContext *ctx = (CodecContext *)arg;
    printf("启动编码器...\n");
    pthread_t thread;
    pthread_create(&thread, NULL, client_thread, &ctx->server_fd);
    AMediaCodecBufferInfo info;
    const int64_t timeout_us = 10000; // 10ms
    fd_set wd_set;
    int maxfd = 0;
    sleep(1);
    ssize_t buf_idx = 0;

    size_t out_size;
    uint8_t *buf = 0;

    start_buf = start;
    ssize_t sent = 0;
    uint8_t *nalu_data = 0;
    int size = 0;
    printf("C++ 纯原生 TCP 发送线程启动。\n");
    // int ok = 0;
    while (ctx->is_running)
    {
        buf_idx = AMediaCodec_dequeueOutputBuffer(ctx->codec, &info, -1);

        if (buf_idx >= 0)
        {
            buf = AMediaCodec_getOutputBuffer(ctx->codec, buf_idx, &out_size);
            /*if (ok == 0&&buf && info.size > 0)
            {
                printf("ok 层捕获到编码数据，大小为 %d\n", info.size);
                ok = 1;
                memcpy(start, buf + info.offset, info.size);
                start_buf_size = info.size;
            }*/
            if (buf && info.size > 0 && client_count > 0)
            {
                FD_ZERO(&wd_set);
                maxfd = 0;
                for (int i = 0; i < client_count; ++i)
                {
                    FD_SET(client_arr[i], &wd_set);
                    if (client_arr[i] > maxfd)
                    {
                        maxfd = client_arr[i];
                    }
                }
                int ret = select(maxfd + 1, 0, &wd_set, 0, 0);
                if (ret < 0)
                {
                    perror("错误 select()");
                }
                nalu_data = buf + info.offset;
                size = info.size;

                // 【彻底使用 C 语言原生发送】：绕过 JVM，无多余内存消耗
                for (int i = 0; i < client_count; i++)
                {
                    if (FD_ISSET(client_arr[i], &wd_set))
                    {
                        sent = sendd(client_arr[i], (char *)&size, sizeof(int), 0);
                        if (sent != sizeof(int))
                        {
                            fprintf(stderr, "TCP %d 客户端似乎断开了连接，发送失败。\n", client_arr[i]);
                            close(client_arr[i]);
                            memcpy(client_arr + i, client_arr + i + 1, sizeof(int) * (client_count - i - 1));
                            client_count--;
                            i--;
                            continue;
                        }
                        sent = sendd(client_arr[i], (char *)nalu_data, info.size, 0);
                        if (sent != info.size)
                        {
                            fprintf(stderr, "TCP %d 客户端似乎断开了连接，发送失败。\n", client_arr[i]);
                            close(client_arr[i]);
                            memcpy(client_arr + i, client_arr + i + 1, sizeof(int) * (client_count - i - 1));
                            client_count--;
                            i--;
                            continue;
                        }
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(ctx->codec, buf_idx, false);
        }
        else if (buf_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            AMediaFormat *format = AMediaCodec_getOutputFormat(ctx->codec);
            printf("C++ 捕获编码格式改变: %s\n", AMediaFormat_toString(format));
            // 【核心修复】：提取 H.264 的首要配置帧（SPS 和 PPS）
            size_t sps_size = 0, pps_size = 0;
            uint8_t *sps_buf = nullptr;
            uint8_t *pps_buf = nullptr;
            char *p = start;
            memcpy(p, &Width, 4);
            p += 4;
            memcpy(p, &Height, 4);
            p += 4;
            int start_buf_size = 8 + 4;
            p+= 4;
            // 从 format 中提取 csd-0 (SPS) 和 csd-1 (PPS)
            if (AMediaFormat_getBuffer(format, "csd-0", (void **)&sps_buf, &sps_size) && sps_buf)
            {
                // 优先把 SPS 发送给电脑端
                // send(ctx->client_fd, sps_buf, sps_size, 0);
                memcpy(p, sps_buf, sps_size);
                p += sps_size;
                start_buf_size += sps_size;
                printf("成功保存  SPS 配置帧，大小: %zu", sps_size);
            }

            if (AMediaFormat_getBuffer(format, "csd-1", (void **)&pps_buf, &pps_size) && pps_buf)
            {
                // 接着把 PPS 发送给电脑端
                // send(ctx->client_fd, pps_buf, pps_size, 0);
                memcpy(p, pps_buf, pps_size);
                p += pps_size;
                start_buf_size += pps_size;
                printf("成功保存 PPS 配置帧，大小: %zu,%d\n", pps_size, start_buf_size);
            }
            memcpy(start + 8 , &start_buf_size, 4);
            AMediaFormat_delete(format);
        }
    }
    printf("C++ 纯原生 TCP 发送线程已安全关闭。\n");
    return nullptr;
}
extern "C" JNIEXPORT void JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_startNativeServerAndEncoder()
{
    codec_output_thread(&g_ctx);
}
// 关键：返回值从 jboolean 改为 jobject (返回创建好的 Java Surface)
extern "C" JNIEXPORT jobject JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_initNativeServerAndEncoder(
    JNIEnv *env, jobject thiz, jint width, jint height, jint bitrate, jint port)
{

    printf("bitrate: %d %dx%d\n", bitrate, width, height);
    Width = width;
    Height = height;
    // 1. 创建、绑定、监听 TCP Socket (保持不变)
    g_ctx.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_ctx.server_fd < 0)
        return nullptr;
    printf("开始线程 %d\n", g_ctx.server_fd);

    int opt = 1;
    setsockopt(g_ctx.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(g_ctx.server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        fprintf(stderr, "绑定失败\n");
        close(g_ctx.server_fd);
        return nullptr;
    }
    if (listen(g_ctx.server_fd, 1) < 0)
    {
        fprintf(stderr, "监听失败\n");

        close(g_ctx.server_fd);
        return nullptr;
    }

    // 2. 率先创建 H.264 编码组件
    g_ctx.codec = AMediaCodec_createEncoderByType("video/avc");
    if (!g_ctx.codec)
    {
        close(g_ctx.server_fd);
        return nullptr;
    }

    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 60);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 3);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 2130708361); // COLOR_FormatSurface
    AMediaFormat_setInt32(format, "profile", 8);                              // H.264 Baseline Profile
    AMediaFormat_setInt32(format, "level", 65536);                            // H.264 Level 3.1
    media_status_t status = AMediaCodec_configure(g_ctx.codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK)
    {
        AMediaCodec_delete(g_ctx.codec);
        close(g_ctx.server_fd);
        return nullptr;
    }

    // 3. 【核心修复】：直接让 AMediaCodec 在 C 层创建 Input Surface (ANativeWindow)
    // 这样创建出来的 Window 内部天生携带着完美的、由多媒体框架初始化的 BufferQueue
    status = AMediaCodec_createInputSurface(g_ctx.codec, &g_ctx.window);
    if (status != AMEDIA_OK || !g_ctx.window)
    {
        printf("C 层 AMediaCodec_createInputSurface 失败\n");
        AMediaCodec_delete(g_ctx.codec);

        close(g_ctx.server_fd);
        return nullptr;
    }

    // 4. 启动编码器
    status = AMediaCodec_start(g_ctx.codec);
    if (status != AMEDIA_OK)
    {
        ANativeWindow_release(g_ctx.window);
        AMediaCodec_delete(g_ctx.codec);
        close(g_ctx.server_fd);
        return nullptr;
    }

    // 5. 启动视频发送线程
    g_ctx.is_running = true;
    // pthread_create(&g_ctx.thread, nullptr, codec_output_thread, &g_ctx);

    // 6. 【关键转换】：利用 NDK 的 ANativeWindow_toSurface 方法，
    // 把 C 层这个功能完备的本地窗口，包装成一个 Java 层的 Surface 对象返回给 Java！
    jobject jSurface = ANativeWindow_toSurface(env, g_ctx.window);
    printf("C++ 原生 TCP 服务器正在监听端口 %d ... 阻塞等待客户端接入\n", port);
    return jSurface;
}

extern "C" JNIEXPORT void JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_stopNativeServerAndEncoder(JNIEnv *env, jobject thiz)
{
    if (g_ctx.is_running)
    {
        g_ctx.is_running = false;
    }
    if (g_ctx.codec)
    {
        AMediaCodec_stop(g_ctx.codec);
        AMediaCodec_delete(g_ctx.codec);
        g_ctx.codec = nullptr;
    }
    if (g_ctx.window)
    {
        ANativeWindow_release(g_ctx.window);
        g_ctx.window = nullptr;
    }

    if (g_ctx.server_fd >= 0)
    {
        close(g_ctx.server_fd);
        g_ctx.server_fd = -1;
    }
    printf("C++ 原生 TCP 服务器和编码器已完全回收。\n");
}
