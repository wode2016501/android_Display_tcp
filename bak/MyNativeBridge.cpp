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
#define TAG "scrcpy_c_networking"
// #define printf(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
// #define fprintf(stderr,...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct CodecContext
{
    AMediaCodec *codec = nullptr;
    ANativeWindow *window = nullptr;
    pthread_t thread;
    bool is_running = false;
    int server_fd = -1;
    int client_fd = -1;
} g_ctx;

// C++ 层的核心循环：不断从硬编码器提取 H.264，并通过 C++ 建立的 TCP Socket 直接发送
void *codec_output_thread(void *arg)
{
    CodecContext *ctx = (CodecContext *)arg;
    AMediaCodecBufferInfo info;
    const int64_t timeout_us = 10000; // 10ms

    printf("C++ 纯原生 TCP 发送线程启动。\n");
    // 【核心修复】：提取 H.264 的首要配置帧（SPS 和 PPS）
    size_t sps_size = 0, pps_size = 0;
    uint8_t *sps_buf = nullptr;
    uint8_t *pps_buf = nullptr;

    // 从 format 中提取 csd-0 (SPS) 和 csd-1 (PPS)
    if (AMediaFormat_getBuffer(format, "csd-0", (void **)&sps_buf, &sps_size) && sps_buf)
    {
        // 优先把 SPS 发送给电脑端
        send(ctx->client_fd, sps_buf, sps_size, 0);
        LOGI("成功向电脑端发送 SPS 配置帧，大小: %zu", sps_size);
    }

    if (AMediaFormat_getBuffer(format, "csd-1", (void **)&pps_buf, &pps_size) && pps_buf)
    {
        // 接着把 PPS 发送给电脑端
        send(ctx->client_fd, pps_buf, pps_size, 0);
        LOGI("成功向电脑端发送 PPS 配置帧，大小: %zu", pps_size);
    }

    while (ctx->is_running)
    {
        ssize_t buf_idx = AMediaCodec_dequeueOutputBuffer(ctx->codec, &info, timeout_us);

        if (buf_idx >= 0)
        {
            size_t out_size;
            uint8_t *buf = AMediaCodec_getOutputBuffer(ctx->codec, buf_idx, &out_size);

            if (buf && info.size > 0)
            {
                uint8_t *nalu_data = buf + info.offset;

                // 【彻底使用 C 语言原生发送】：绕过 JVM，无多余内存消耗
                ssize_t sent = send(ctx->client_fd, nalu_data, info.size, 0);
                if (sent < 0)
                {
                    fprintf(stderr, "TCP 客户端似乎断开了连接，发送失败。\n");
                    ctx->is_running = false;
                }
            }
            AMediaCodec_releaseOutputBuffer(ctx->codec, buf_idx, false);
        }
        else if (buf_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            AMediaFormat *format = AMediaCodec_getOutputFormat(ctx->codec);
            printf("C++ 捕获编码格式改变: %s\n", AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        }
    }
    printf("C++ 纯原生 TCP 发送线程已安全关闭。\n");
    return nullptr;
}
// 关键：返回值从 jboolean 改为 jobject (返回创建好的 Java Surface)
extern "C" JNIEXPORT jobject JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_startNativeServerAndEncoder(
    JNIEnv *env, jobject thiz, jint width, jint height, jint bitrate, jint port)
{

    printf("bitrate: %d %dx%d\n", bitrate, width, height);
    // 1. 创建、绑定、监听 TCP Socket (保持不变)
    g_ctx.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_ctx.server_fd < 0)
        return nullptr;

    int opt = 1;
    setsockopt(g_ctx.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(g_ctx.server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        close(g_ctx.server_fd);
        return nullptr;
    }
    if (listen(g_ctx.server_fd, 1) < 0)
    {
        close(g_ctx.server_fd);
        return nullptr;
    }

    printf("C++ 原生 TCP 服务器正在监听端口 %d ... 阻塞等待客户端接入\n", port);
    int addrlen = sizeof(address);
    g_ctx.client_fd = accept(g_ctx.server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
    if (g_ctx.client_fd < 0)
    {
        close(g_ctx.server_fd);
        return nullptr;
    }

    printf("电脑端客户端已接入 C++ TCP 通道！现在启动编码器...\n");

    // 2. 率先创建 H.264 编码组件
    g_ctx.codec = AMediaCodec_createEncoderByType("video/avc");
    if (!g_ctx.codec)
    {
        close(g_ctx.client_fd);
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

    media_status_t status = AMediaCodec_configure(g_ctx.codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK)
    {
        AMediaCodec_delete(g_ctx.codec);
        close(g_ctx.client_fd);
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
        close(g_ctx.client_fd);
        close(g_ctx.server_fd);
        return nullptr;
    }

    // 4. 启动编码器
    status = AMediaCodec_start(g_ctx.codec);
    if (status != AMEDIA_OK)
    {
        ANativeWindow_release(g_ctx.window);
        AMediaCodec_delete(g_ctx.codec);
        close(g_ctx.client_fd);
        close(g_ctx.server_fd);
        return nullptr;
    }

    // 5. 启动视频发送线程
    g_ctx.is_running = true;
    pthread_create(&g_ctx.thread, nullptr, codec_output_thread, &g_ctx);

    // 6. 【关键转换】：利用 NDK 的 ANativeWindow_toSurface 方法，
    // 把 C 层这个功能完备的本地窗口，包装成一个 Java 层的 Surface 对象返回给 Java！
    jobject jSurface = ANativeWindow_toSurface(env, g_ctx.window);
    return jSurface;
}

extern "C" JNIEXPORT void JNICALL
Java_com_my_scrcpy_binding_MyNativeBridge_stopNativeServerAndEncoder(JNIEnv *env, jobject thiz)
{
    if (g_ctx.is_running)
    {
        g_ctx.is_running = false;
        pthread_join(g_ctx.thread, nullptr);
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
    if (g_ctx.client_fd >= 0)
    {
        close(g_ctx.client_fd);
        g_ctx.client_fd = -1;
    }
    if (g_ctx.server_fd >= 0)
    {
        close(g_ctx.server_fd);
        g_ctx.server_fd = -1;
    }
    printf("C++ 原生 TCP 服务器和编码器已完全回收。\n");
}
