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
// #define printf(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
// #define fprintf(stderr,...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define MAX_CLIENT_COUNT 666
int *client_arr = 0;
int client_count = 0;
int client_status = 0;
char *start_buf = 0;
int start_buf_size = 0;
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

void *client_thread(void *arg)
{
    int clientfd[MAX_CLIENT_COUNT];
    client_arr = clientfd;
    int server_fd = *(int *)arg;
    printf("启动客户端线程 %d\n", server_fd);
    struct sockaddr_in6 address;
    int addrlen = sizeof(address);
    int client_fd = 0;
    int ret = 0;
    char buf[255];
    int *py = 0;

    while (1)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);

        if (client_fd < 0)
        {
            printf("客户端连接失败\n");
            continue;
        }
		// ⚠️ 过滤回环地址逻辑（同时拦截 IPv4 和 IPv6 的本地回环）
		
		// 情况 A：纯 IPv6 本地回环地址，即 "::1"
		int is_v6_loopback = (IN6_IS_ADDR_LOOPBACK(&address.sin6_addr));
		
		// 情况 B：IPv4 映射到 IPv6 的本地回环地址，即 "::ffff:127.0.0.1"
		int is_v4_mapped_loopback = (IN6_IS_ADDR_V4MAPPED(&address.sin6_addr) && 
									 address.sin6_addr.s6_addr[12] == 127 && 
									 address.sin6_addr.s6_addr[13] == 0 && 
									 address.sin6_addr.s6_addr[14] == 0 && 
									 address.sin6_addr.s6_addr[15] == 1);

		if (is_v6_loopback || is_v4_mapped_loopback)
		{
			printf("⚠️ 拦截到本地回环地址连接（IPv4/IPv6），已自动断开。\n");
			close(client_fd);
			continue;
		}

		// 打印连接信息（兼容 IPv4 和 IPv6 格式的打印）
		char ip_str[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &address.sin6_addr, ip_str, sizeof(ip_str));
		printf("✓ 已连接到发送端: %s\n", ip_str);

        client_arr[client_count++] = client_fd;
        while (start_buf_size == 0)
        {
            sleep(1);
        }
        py = (int *)start_buf;
        printf("====%dx%d size: %d %d\n", py[0], py[1], py[2], start_buf_size);
        ret = sendd(client_fd, start_buf, start_buf_size, 0);
        if (ret != start_buf_size)
        {
            printf("发送失败\n");
            close(client_fd);
            client_count--;
            continue;
        }
        client_status = 1;
        printf("客户端连接成功 %d count=%d\n", client_fd, client_count);
        // 1. 设置发送超时 (例如 1 秒)
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    return nullptr;
}
// C++ 层的核心循环：不断从硬编码器提取 H.264，并通过 C++ 建立的 TCP Socket 直接发送
void *codec_output_thread(void *arg)
{
    char start[255];
    CodecContext *ctx = (CodecContext *)arg;
    printf("启动编码器...\n");
    AMediaCodecBufferInfo info;
    const int64_t timeout_us = 10000; // 10ms
    fd_set wd_set;
    int maxfd = 0;
    // sleep(1);
    ssize_t buf_idx = 0;

    size_t out_size;
    uint8_t *buf = 0;

    start_buf = start;
    ssize_t sent = 0;
    uint8_t *nalu_data = 0;
    int size = 0;
    printf("C++ 纯原生 TCP 发送线程启动。\n");
    while (client_count > 0)
    {
        buf_idx = AMediaCodec_dequeueOutputBuffer(ctx->codec, &info, -1);

        if (buf_idx >= 0)
        {
            buf = AMediaCodec_getOutputBuffer(ctx->codec, buf_idx, &out_size);
            if (buf && info.size > 0 && client_status != 0)
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
            int buf_size = 8 + 4;
            p += 4;

            // 从 format 中提取 csd-0 (SPS) 和 csd-1 (PPS)
            if (AMediaFormat_getBuffer(format, "csd-0", (void **)&sps_buf, &sps_size) && sps_buf)
            {
                // 优先把 SPS 发送给电脑端
                // send(ctx->client_fd, sps_buf, sps_size, 0);
                memcpy(p, sps_buf, sps_size);
                p += sps_size;
                buf_size += sps_size;
                printf("成功保存  SPS 配置帧，大小: %zu", sps_size);
            }

            if (AMediaFormat_getBuffer(format, "csd-1", (void **)&pps_buf, &pps_size) && pps_buf)
            {
                // 接着把 PPS 发送给电脑端
                // send(ctx->client_fd, pps_buf, pps_size, 0);
                memcpy(p, pps_buf, pps_size);
                p += pps_size;
                buf_size += pps_size;
                printf("成功保存 PPS 配置帧，大小: %zu,%d\n", pps_size, buf_size);
            }
            pps_size += sps_size;
            memcpy(start + 8, &pps_size, 4);
            start_buf_size = buf_size;
            AMediaFormat_delete(format);
        }
    }
    client_status = 0;
    start_buf_size = 0;
    AMediaCodec_stop(g_ctx.codec);
    AMediaCodec_delete(g_ctx.codec);
    g_ctx.codec = nullptr;
    printf("C++ 纯原生 TCP 发送线程已安全关闭。\n");
    return nullptr;
}
extern "C" JNIEXPORT void JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_startNativeServerAndEncoder(JNIEnv *env, jobject thiz)
{
    codec_output_thread(&g_ctx);
}
extern "C" JNIEXPORT int JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_setNativeServerAndEncoder(
    JNIEnv *env, jobject thiz, jint port)
{
    if (g_ctx.server_fd < 0)
    {
        printf("开始监听端口 %d\n", port);
        // 1. 创建、绑定、监听 TCP Socket (保持不变)
        g_ctx.server_fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (g_ctx.server_fd < 0)
            return 0;
        printf("开始线程 %d\n", g_ctx.server_fd);
        int opt = 1;
        setsockopt(g_ctx.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        	// ⚠️ 关键修改 3：关闭 IPV6_V6ONLY 选项（允许该 IPv6 Socket 接收 IPv4 连接）
	int v6only = 0;
	if (setsockopt(g_ctx.server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
	{
		perror("关闭 IPV6_V6ONLY 失败");
		close(g_ctx.server_fd);
		return 1;
	}
        struct sockaddr_in6 address;
        memset(&address, 0, sizeof(address));
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);

        if (bind(g_ctx.server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
        {
            fprintf(stderr, "绑定失败\n");
            close(g_ctx.server_fd);
            g_ctx.server_fd = -1;
            return 0;
        }
        if (listen(g_ctx.server_fd, 1) < 0)
        {
            fprintf(stderr, "监听失败\n");

            close(g_ctx.server_fd);
            g_ctx.server_fd = -1;
            return 0;
        }
        pthread_t thread;
        pthread_create(&thread, NULL, client_thread, &g_ctx.server_fd);
    }
    return g_ctx.server_fd;
}
// 关键：返回值从 jboolean 改为 jobject (返回创建好的 Java Surface)
extern "C" JNIEXPORT jobject JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_initNativeServerAndEncoder(
    JNIEnv *env, jobject thiz, jint width, jint height, jint bitrate)
{
    if (g_ctx.server_fd < 0)
    {
        return nullptr;
    }
    while (client_count <= 0)
    {
        sleep(1);
    }

    printf("bitrate: %d %dx%d\n", bitrate, width, height);
    Width = width;
    Height = height;

    // 2. 率先创建 H.264 编码组件
    g_ctx.codec = AMediaCodec_createEncoderByType("video/avc");
    if (!g_ctx.codec)
    {
        return nullptr;
    }

    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, Width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, Height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 60);
    // max-fps
    //  AMediaFormat_setFloat(format, "max-fps-to-encoder", 60.0f);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);      // 关键：I 帧间隔为 1 秒
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 2130708361); // COLOR_FormatSurface
    AMediaFormat_setInt32(format, "profile", 8);                              // H.264 Baseline Profile
    AMediaFormat_setInt32(format, "level", 65536);                            // H.264 Level 3.1
    media_status_t status = AMediaCodec_configure(g_ctx.codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK)
    {
        AMediaCodec_delete(g_ctx.codec);
        fprintf(stderr, "AMediaCodec_configure 失败\n");
        return nullptr;
    }

    // 3. 【核心修复】：直接让 AMediaCodec 在 C 层创建 Input Surface (ANativeWindow)
    // 这样创建出来的 Window 内部天生携带着完美的、由多媒体框架初始化的 BufferQueue
    status = AMediaCodec_createInputSurface(g_ctx.codec, &g_ctx.window);
    if (status != AMEDIA_OK || !g_ctx.window)
    {
        fprintf(stderr, "C 层 AMediaCodec_createInputSurface 失败\n");
        AMediaCodec_delete(g_ctx.codec);

        return nullptr;
    }

    // 4. 启动编码器
    status = AMediaCodec_start(g_ctx.codec);
    if (status != AMEDIA_OK)
    {
        ANativeWindow_release(g_ctx.window);
        AMediaCodec_delete(g_ctx.codec);
        fprintf(stderr, "AMediaCodec_start 失败\n");
        return nullptr;
    }

    // 5. 启动视频发送线程
    g_ctx.is_running = true;
    // pthread_create(&g_ctx.thread, nullptr, codec_output_thread, &g_ctx);

    // 6. 【关键转换】：利用 NDK 的 ANativeWindow_toSurface 方法，
    // 把 C 层这个功能完备的本地窗口，包装成一个 Java 层的 Surface 对象返回给 Java！
    jobject jSurface = ANativeWindow_toSurface(env, g_ctx.window);
    printf("C++ 编码器准备就绪\n");
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
    start_buf_size = 0;
    /*
        if (g_ctx.server_fd >= 0)
        {
            close(g_ctx.server_fd);
            g_ctx.server_fd = -1;
        }
        printf("C++ 原生和编码器已完全回收。\n");
        */
}
