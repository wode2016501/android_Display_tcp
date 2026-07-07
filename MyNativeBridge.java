package com.my.scrcpy.binding;

import android.view.Surface;
import android.os.IBinder;
import android.graphics.Rect;
import java.io.IOException;
import java.lang.reflect.Constructor;

public class MyNativeBridge {
    static {
        System.loadLibrary("my_scrcpy_codec");
    }

    // JNI 声明：让 C 语言在 6666 端口启动监听，并准备好编码器
    private native Surface initNativeServerAndEncoder(int width, int height, int bitrate);

    private native int setNativeServerAndEncoder(int port);

    private native void startNativeServerAndEncoder();

    // 修改后，与第 54 行及 C++ 层的函数名完全对齐
    private native void stopNativeServerAndEncoder();

    private IBinder displayToken;
    private Surface mySurface;

    public void startMirroring(int width, int height, int bitrate, int port) throws IOException {
        stopMirroring();
        if(setNativeServerAndEncoder(port)<1){
            throw new IOException("C 层服务器启动失败！");
        };
        // 1. 【调用 C++】：让 C++ 去创建带 BufferQueue 的高品质 Surface
        // 这一步在电脑端连接 6666 端口之前依然会阻塞
        mySurface = initNativeServerAndEncoder(width, height, bitrate);

        if (mySurface == null) {
            throw new IOException("C 硬件编码器 InputSurface 创建失败！");
        }

        System.out.println("C 层 Surface 创建成功，正在通过隐藏 API 绑定虚拟屏幕...");

        // 2. 绑定系统虚拟显示器到这个从 C++ 递过来的 mySurface 上
        try {
            displayToken = reflectCreateDisplay("my_display", false);
            reflectOpenTransaction();
            try {
                reflectSetDisplaySurface(displayToken, mySurface);
                Rect displayRect = new Rect(0, 0, width, height);
                reflectSetDisplayProjection(displayToken, 0, displayRect, displayRect);
                reflectSetDisplayLayerStack(displayToken, 0);
            } finally {
                reflectCloseTransaction();
            }
        } catch (Exception e) {
            stopMirroring();
            throw new IOException("Android 录屏配置失败", e);
        }
        startNativeServerAndEncoder();
    }

    public void stopMirroring() {
        if (displayToken != null) {
            try {
                reflectDestroyDisplay(displayToken);
            } catch (Exception ignored) {
            }
            displayToken = null;
        }
        try {
            stopNativeServerAndEncoder();
        } catch (Exception ignored) {
        }
        if (mySurface != null) {
            mySurface.release();
            mySurface = null;
        }
    }

    // --- 以下为纯反射系统隐藏 API 逻辑（保持不变） ---
    private static IBinder reflectCreateDisplay(String name, boolean secure) throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("createDisplay", String.class, boolean.class);
        method.setAccessible(true);
        return (IBinder) method.invoke(null, name, secure);
    }

    private static void reflectOpenTransaction() throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("openTransaction");
        method.setAccessible(true);
        method.invoke(null);
    }

    private static void reflectCloseTransaction() throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("closeTransaction");
        method.setAccessible(true);
        method.invoke(null);
    }

    private static void reflectSetDisplaySurface(IBinder displayToken, Surface surface) throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("setDisplaySurface", IBinder.class, Surface.class);
        method.setAccessible(true);
        method.invoke(null, displayToken, surface);
    }

    private static void reflectSetDisplayProjection(IBinder displayToken, int orientation, Rect layerStackRect,
            Rect displayRect) throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("setDisplayProjection", IBinder.class, int.class,
                Rect.class, Rect.class);
        method.setAccessible(true);
        method.invoke(null, displayToken, orientation, layerStackRect, displayRect);
    }

    private static void reflectSetDisplayLayerStack(IBinder displayToken, int layerStack) throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("setDisplayLayerStack", IBinder.class, int.class);
        method.setAccessible(true);
        method.invoke(null, displayToken, layerStack);
    }

    private static void reflectDestroyDisplay(IBinder displayToken) throws Exception {
        Class<?> cls = Class.forName("android.view.SurfaceControl");
        java.lang.reflect.Method method = cls.getDeclaredMethod("destroyDisplay", IBinder.class);
        method.setAccessible(true);
        method.invoke(null, displayToken);
    }

    // 独立可执行入口
    public static void main(String[] args) {
        System.out.println("[Java] 本地 C 架构投屏服务启动...");
        MyNativeBridge bridge = new MyNativeBridge();
        try {
            while (true) {
            // 一键启动：内部会直接调用 C 语言在 6666 端口进行 TCP 监听
            bridge.startMirroring(1920, 1080, 8000000,9999);
            System.out.println("[Java] 服务已完美跑在 C++ 传输层，输入 Ctrl+C 退出进程。");

            // 保持 Java 守护进程存活
            // while (true) {
            // Thread.sleep(5000);
            // }
            bridge.stopMirroring();
            }

        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            // bridge.stopMirroring();
        }
    }
}
