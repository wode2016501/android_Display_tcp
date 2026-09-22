// unified_receiver.c - 统一接收端（设备创建 + 网络接收）
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <linux/time.h>
// #define printf(...) printf( LOG_TAG, __VA_ARGS__)
// #define fprintf(stderr,...) fprintf(stderr, LOG_TAG, __VA_ARGS__)
// #define printf(...) printf(  __VA_ARGS__)
#define SLOT_MAX 25
char iID[SLOT_MAX];
int eventCount = 0;
#define PORT 9000

typedef struct
{
	int SCREEN_WIDTH;
	int SCREEN_HEIGHT;
	char n;
} tinfo;
tinfo tinfo1 = {1080, 2340, '\n'};
// ==================== 触摸点管理 ====================
typedef struct
{
	int id;
	int x;
	int y;
	int active;
} TouchPoint;

// ==================== 按键点管理 ====================
typedef struct
{
	int keyCode;
	int active;
} KeyPoint;

// ==================== 自定义固定大小结构体 ====================
typedef struct
{
	long long tv_sec;
	long long tv_usec;
	unsigned short type;
	unsigned short code;
	unsigned int value;
} input_event_test;

// 全局变量
static int uinput_fd = -1;
int event_count = 0;
static int server_socket = -1;
static int running = 1;
// static TouchPoint touchPoints[100];
static int touchCount = 0;
int client_count = 0;
// static pthread_mutex_t touchMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t uinputMutex = PTHREAD_MUTEX_INITIALIZER;

// 延时
struct idtime
{
	struct timespec prev_ts;
	struct timespec now;
};

// ==================== 注入事件到虚拟设备 ====================
int inject_event(int uinput_fd, input_event_test *ev)
{
	if (uinput_fd < 0)
	{
		return -1;
	}
	// pthread_mutex_lock(&uinputMutex);
	int ret = write(uinput_fd, ev, sizeof(struct input_event));
	// pthread_mutex_unlock(&uinputMutex);
	if (ret != sizeof(struct input_event))
	{
		perror("写入设备失败");
		running = 0; // 停止接收数据
		return -1;
	}
	return 0;
}

// 使用自定义结构体发送
int send_input_event_test(int type, int code, int value)
{
	input_event_test test_ev;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	test_ev.tv_sec = tv.tv_sec;
	test_ev.tv_usec = tv.tv_usec;
	test_ev.type = type;
	test_ev.code = code;
	test_ev.value = value;
	if (inject_event(uinput_fd, &test_ev) != 0)
		return -1;
	return 0;
}

// 发送触摸事件（带坐标转换）
void send_touch_event(int id, int x, int y, int action)
{
	if (action == 0)
	{ // 按下
		send_input_event_test(EV_ABS, ABS_MT_SLOT, id);
		send_input_event_test(EV_ABS, ABS_MT_TRACKING_ID, id);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_X, x);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_Y, y);
		send_input_event_test(EV_KEY, BTN_TOUCH, 1);
	}
	else if (action == 1)
	{ // 移动
		send_input_event_test(EV_ABS, ABS_MT_SLOT, id);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_X, x);
		send_input_event_test(EV_ABS, ABS_MT_POSITION_Y, y);
	}
	else if (action == 2)
	{ // 抬起
		send_input_event_test(EV_ABS, ABS_MT_SLOT, id);
		send_input_event_test(EV_ABS, ABS_MT_TRACKING_ID, -1);

		// printf("eventCount :%d\n", eventCount);

		// if (eventCount == 0)
		//	send_input_event_test(EV_KEY, BTN_TOUCH, 0);
	}

	// SYN_REPORT
	// send_input_event_test(EV_SYN, SYN_REPORT, 0);
}

// 发送按键事件
void send_key_event(int keyCode, int action)
{
	send_input_event_test(EV_KEY, keyCode, action);
	send_input_event_test(EV_SYN, SYN_REPORT, 0);
	// printf("发送按键: code=%d, action=%s", keyCode, action ? "DOWN" : "UP");
}

// ==================== 启用所有按键 ====================
void enable_all_keys(int fd)
{

	for (int key = 0; key <= 248; key++)
	{
		ioctl(fd, UI_SET_KEYBIT, key);
	}
	/*
	// 字母键 A-Z
	for (int key = KEY_A; key <= KEY_Z; key++)
	{
	ioctl(fd, UI_SET_KEYBIT, key);
	}

	// 数字键 0-9
	for (int key = KEY_0; key <= KEY_9; key++)
	{
	ioctl(fd, UI_SET_KEYBIT, key);
	}

	// 功能键
	ioctl(fd, UI_SET_KEYBIT, KEY_ENTER);
	ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
	ioctl(fd, UI_SET_KEYBIT, KEY_BACKSPACE);
	ioctl(fd, UI_SET_KEYBIT, KEY_TAB);
	ioctl(fd, UI_SET_KEYBIT, KEY_ESC);
	ioctl(fd, UI_SET_KEYBIT, KEY_DELETE);

	// 方向键
	ioctl(fd, UI_SET_KEYBIT, KEY_UP);
	ioctl(fd, UI_SET_KEYBIT, KEY_DOWN);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHT);

	// 系统键
	ioctl(fd, UI_SET_KEYBIT, KEY_HOME);
	ioctl(fd, UI_SET_KEYBIT, KEY_BACK);
	ioctl(fd, UI_SET_KEYBIT, KEY_MENU);
	ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP);
	ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
	ioctl(fd, UI_SET_KEYBIT, KEY_POWER);
	ioctl(fd, UI_SET_KEYBIT, KEY_CAMERA);

	// 修饰键
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTSHIFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTCTRL);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTALT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTALT);
	*/
	printf("✓ 已启用所有按键\n");
}

// ==================== 创建虚拟设备 ====================
int create_virtual_device()
{
	int fd;
	struct uinput_user_dev uidev;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
	{
		perror("打开 /dev/uinput 失败");
		return -1;
	}

	printf("配置虚拟输入设备...\n");

	// 1. 配置触摸事件
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);

	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);

	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

	// 2. 配置按键事件
	enable_all_keys(fd);

	// 3. 配置设备参数
	memset(&uidev, 0, sizeof(uidev));
	strcpy(uidev.name, "Virtual Touch + Keyboard");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor = 0x0eef;
	uidev.id.product = 0x0002;
	uidev.id.version = 1;

	// 触摸坐标范围
	uidev.absmin[ABS_MT_POSITION_X] = 0;
	uidev.absmax[ABS_MT_POSITION_X] = tinfo1.SCREEN_WIDTH - 1;
	uidev.absmin[ABS_MT_POSITION_Y] = 0;
	uidev.absmax[ABS_MT_POSITION_Y] = tinfo1.SCREEN_HEIGHT - 1;
	uidev.absmin[ABS_MT_SLOT] = 0;
	uidev.absmax[ABS_MT_SLOT] = SLOT_MAX; // 最大支持10个触点
	uidev.absmin[ABS_MT_TRACKING_ID] = 0;
	uidev.absmax[ABS_MT_TRACKING_ID] = 65535;

	write(fd, &uidev, sizeof(uidev));

	if (ioctl(fd, UI_DEV_CREATE) < 0)
	{
		perror("创建设备失败");
		close(fd);
		return -1;
	}

	printf("\n========================================\n");
	printf("✓ 虚拟设备创建成功！\n");
	printf("  设备名称: Virtual Touch + Keyboard\n");
	printf("  分辨率: %dx%d\n", tinfo1.SCREEN_WIDTH, tinfo1.SCREEN_HEIGHT);
	printf("  支持: 触摸屏 + 键盘按键\n");
	printf("========================================\n\n");

	return fd;
}
int read_(int fd, char *buf, size_t size, int max_size)
{
	if (size > max_size)
		return -1;
	int y = size;
	int ret = 0;
	while (y > 0)
	{
		ret = read(fd, buf, y);
		if (ret < 0)
			return -1;
		if (ret == 0)
			return -1;
		buf += ret;
		y -= ret;
	}
	return size;
}

// 全局或线程内静态变量，记录上一次成功发送 SYN_REPORT 的绝对时间
static struct timespec last_syn_time = {0, 0};

void send_syn_report_safebak(int uinput_fd)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (last_syn_time.tv_sec != 0 || last_syn_time.tv_nsec != 0)
	{
		// 计算距离上一次发送 SYN 过去了多少纳秒
		long long elapsed_ns = (now.tv_sec - last_syn_time.tv_sec) * 1000000000LL +
							   (now.tv_nsec - last_syn_time.tv_nsec);

		// 120Hz 的标准间隔是 8333333 纳秒 (如果是60Hz则是 16666667)
		long long target_interval = 8333333;

		if (elapsed_ns < target_interval)
		{
			// 如果发送太快（比如由于网络粘包连续涌入），强制精确休眠补齐剩下的时间
			struct timespec delay;
			long long sleep_ns = target_interval - elapsed_ns;
			delay.tv_sec = 0;
			delay.tv_nsec = sleep_ns;
			nanosleep(&delay, NULL);

			// 睡眠后重新获取当前准确时间
			clock_gettime(CLOCK_MONOTONIC, &now);
		}
	}

	// 真正注入 SYN_REPORT
	send_input_event_test(EV_SYN, SYN_REPORT, 0);

	// 更新上一次发送的时间戳
	last_syn_time = now;
}

int send_syn_report_safe(int w)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (last_syn_time.tv_sec != 0 && last_syn_time.tv_nsec != 0 && w == 0)
	{
		// 计算距离上一次发送 SYN 过去了多少纳秒
		long long elapsed_ns = (now.tv_sec - last_syn_time.tv_sec) * 1000000000LL +
							   (now.tv_nsec - last_syn_time.tv_nsec);

		// 120Hz 的标准间隔是 8333333 纳秒 (如果是60Hz则是 16666667)
		long long target_interval = 8333333;
		if (elapsed_ns > target_interval)
		{
			// 真正注入 SYN_REPORT
			send_input_event_test(EV_SYN, SYN_REPORT, 0);
			// 更新上一次发送的时间戳
			last_syn_time = now;
			return 1;
		}
		return 0;
	}
	else
	{
		if (w)
		{
			usleep(10333);
		}
		send_input_event_test(EV_SYN, SYN_REPORT, 0);
		last_syn_time = now;

		return 1;
	}

	/*if (elapsed_ns < target_interval)
	{
		// 如果发送太快（比如由于网络粘包连续涌入），强制精确休眠补齐剩下的时间
		struct timespec delay;
		long long sleep_ns = target_interval - elapsed_ns;
		delay.tv_sec = 0;
		delay.tv_nsec = sleep_ns;
		nanosleep(&delay, NULL);

		// 睡眠后重新获取当前准确时间
		clock_gettime(CLOCK_MONOTONIC, &now);
	}
	// 真正注入 SYN_REPORT
		send_input_event_test(EV_SYN, SYN_REPORT, 0);

	// 更新上一次发送的时间戳
	last_syn_time = now;
}*/
	/*
	if (elapsed_ns > target_interval)
		{
			// 真正注入 SYN_REPORT
			send_input_event_test(EV_SYN, SYN_REPORT, 0);
			last_syn_time = now;
		}
	}
	else{
		// 真正注入 SYN_REPORT
		send_input_event_test(EV_SYN, SYN_REPORT, 0);
		// 更新上一次发送的时间戳
		last_syn_time = now;
	}*/
}

// ==================== 接收并注入事件 ====================
void *receive_thread(void *arg)
{
	int client_fd = *(int *)arg;
	ssize_t bytes_read;
	TouchPoint tp;
	KeyPoint kp;
	printf("开始接收触摸事件并注入到虚拟设备...\n\n");
	int size = 0;
	int eventmax = 0;
	// struct idtime tidt[10];
	// struct idtime *idt = tidt; // 指向数组首元素
	struct timespec prev_ts[10];
	struct timespec now;
	memset(prev_ts, 0, sizeof(prev_ts)); // 所有成员初始为0
	long long interval_us = 0;
	char id[10];
	char newid[10];
	int iid = 0;
	memset(id, 0, sizeof(id));
	memset(newid, 0, sizeof(newid));
	client_count++;
	int sync_w = 1;
	int ret = 0;
	int new = 1;
	while (running)
	{
		new = 1;
		sync_w = 0;
		bytes_read = read_(client_fd, (char *)&size, sizeof(int), sizeof(int));
		if (bytes_read < 1)
			break;
		if (size == sizeof(TouchPoint))
		{

			bytes_read = read_(client_fd, (char *)&tp, sizeof(TouchPoint), sizeof(TouchPoint));
			if (bytes_read == sizeof(TouchPoint))
			{
				if (tp.id > 10)
					continue;
				if (tp.id < 0)
					continue;
				if (newid[tp.id] == 1 && tp.active == 1)
					new = 0;
				/*if (tp.id < 10)
				{
					clock_gettime(CLOCK_MONOTONIC, &now);
					interval_us = (now.tv_sec - prev_ts[tp.id].tv_sec) * 1000000LL +
								  (now.tv_nsec - prev_ts[tp.id].tv_nsec) / 1000;
					prev_ts[tp.id] = now;
					if (interval_us < 8000 && tp.active == 1)
					{
						continue;
					}
					if (interval_us < 8000 && tp.active == 2)
					{
						//	continue;
						usleep(6000);
					}
				}*/

				if (tp.active == 0 && eventCount < sizeof(iID))
				{
					//printf("按下id=%d ", tp.id);
					if (id[tp.id] != 0)
					{
						fprintf(stderr, "id=%d 重复按下\n", id[tp.id]);
						continue;
					}
					for (int i = 0; i < sizeof(iID); i++)
					{
						if (iID[i] == 0)
						{
							iID[i] = 1;
							id[tp.id] = i + 1;
							break;
						}
					}
					eventCount++;
					//printf("分配id=%d eventCount=%d\n", id[tp.id], eventCount);
				}
				iid = id[tp.id];
				if (tp.active == 1 && iid < 1)
				{
					fprintf(stderr, "iid 0<%d mov不存在\n", id[tp.id]);
					continue;
				}
				if (tp.active == 2)
				{
					//printf("释放id=%d,%d,%d\n", id[tp.id], id[tp.id] - 1, tp.id);
					if (id[tp.id] == 0)
					{
						fprintf(stderr, "id=%d 不存在\n", id[tp.id]);
						continue;
					}
					iID[id[tp.id] - 1] = 0;
					id[tp.id] = 0;
					eventCount--;
					if (eventCount < 0)
					{
						fprintf(stderr, "未知错误: eventCount :%d < 0\n", eventCount);
						running = 0;
						break;
						// send_input_event_test(EV_KEY, BTN_TOUCH, 0);
						// eventCount = 0;
					}
					sync_w = 1;
				}

				// printf("接收触摸事件: id=%d %d, x=%d, y=%d, action=%d,%lldms\n", tp.id, iid, tp.x, tp.y, tp.active, interval_us);
				pthread_mutex_lock(&uinputMutex);

				if (new)
				{
					//if (tp.active == 1)
					//	printf("new\n");
					send_touch_event(iid, tp.x, tp.y, tp.active);
				}
				// send_input_event_test(EV_SYN, SYN_REPORT, 0);
				ret = send_syn_report_safe(sync_w);
				if (ret == 1)
				{
					//printf("zero newid\n");
					memset(newid, 0, sizeof(newid));
				}
				pthread_mutex_unlock(&uinputMutex);
				if (tp.active == 1 && newid[tp.id] == 0 && ret == 0)
				{
					newid[tp.id] = 1;
				}
				continue;
			}

			fprintf(stderr, "读取触摸事件失败\n");
			break;
		}
		if (size == sizeof(KeyPoint))
		{
			bytes_read = read_(client_fd, (char *)&kp, sizeof(KeyPoint), sizeof(KeyPoint));
			if (bytes_read == sizeof(KeyPoint))
			{
				// printf("接收按键事件: keyCode=%d, action=%d\n", kp.keyCode, kp.active);
				pthread_mutex_lock(&uinputMutex);
				send_key_event(kp.keyCode, kp.active);
				pthread_mutex_unlock(&uinputMutex);
				continue;
			}

			fprintf(stderr, "读取按键事件失败\n");
			break;
		}
		fprintf(stderr, "读取size=%d\n", size);
		break;
	}
	for (int i = 0; i < sizeof(id); i++)
	{
		if (id[i] != 0)
		{
			fprintf(stderr, "还有id=%d 没有释放\n", id[i]);
			send_touch_event(id[i], 0, 0, 2);
			iID[id[i] - 1] = 0;
			id[i] = 0;
		}
	}
	client_count--;
	fprintf(stderr, "客户端为%d,发送退出事件\n", client_count);
	if (client_count < 1)
	{
		//	fprintf(stderr, "客户端为%d,发送退出事件\n", client_count);
		send_input_event_test(EV_KEY, BTN_TOUCH, 0);
		memset(iID, 0, sizeof(iID));
		eventCount = 0;
	}

	printf("接收线程退出 %d\n", client_fd);
	close(client_fd);
	pthread_exit(NULL);
	return NULL;
}

// ==================== 信号处理 ====================
void signal_handler(int sig)
{
	printf("\n收到信号 %d，正在退出...\n", sig);
	running = 0;
	if (server_socket >= 0)
	{
		close(server_socket);
	}
}

// ==================== 主函数 ====================
int main(int argc, char **argv)
{
	if (argc == 3)
	{
		tinfo1.SCREEN_WIDTH = atoi(argv[1]);
		tinfo1.SCREEN_HEIGHT = atoi(argv[2]);
	}
	int client_fd;

	// ⚠️ 关键修改 1：将客户端地址改为 sockaddr_in6 以支持 IPv6 和双栈
	struct sockaddr_in6 server_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);
	pthread_t recv_thread;
	memset(iID, 0, sizeof(iID)); // 初始化数组
	printf("========================================\n");
	printf("统一接收端（设备创建 + 网络接收）\n");
	printf("分辨率%dx%d\n", tinfo1.SCREEN_WIDTH, tinfo1.SCREEN_HEIGHT);
	printf("========================================\n\n");

	// 设置信号处理
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	// 忽略 SIGPIPE 信号，防止客户端断开时程序闪退
	signal(SIGPIPE, SIG_IGN);

	// ⚠️ 关键修改 2：协议族改为 AF_INET6
	server_socket = socket(AF_INET6, SOCK_STREAM, 0);
	if (server_socket < 0)
	{
		perror("创建 socket 失败");
		return 1;
	}

	// 设置端口重用
	int opt = 1;
	setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// ⚠️ 关键修改 3：关闭 IPV6_V6ONLY 选项（允许该 IPv6 Socket 接收 IPv4 连接）
	int v6only = 0;
	if (setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
	{
		perror("关闭 IPV6_V6ONLY 失败");
		close(server_socket);
		return 1;
	}

	// ⚠️ 关键修改 4：绑定地址结构体改为 IPv6 格式
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_addr = in6addr_any; // 监听所有本地 IPv4 和 IPv6 地址
	server_addr.sin6_port = htons(PORT);

	if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("绑定失败");
		close(server_socket);
		return 1;
	}

	// 监听
	if (listen(server_socket, 5) < 0)
	{
		perror("监听失败");
		close(server_socket);
		return 1;
	}

	printf("等待发送端连接（支持 IPv4/IPv6 双栈），端口: %d...\n", PORT);

	// 1. 创建虚拟设备
	uinput_fd = create_virtual_device();
	if (uinput_fd < 0)
	{
		close(server_socket);
		return 1;
	}

	// 显示设备节点
	system("ls -l /dev/input/event* 2>/dev/null | tail -1");
	printf("\n");

	// 接受连接
	while (running)
	{
		client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0)
		{
			perror("接受连接失败");
			close(server_socket);
			running = 0;
			break;
		}
		// ⚠️ 过滤回环地址逻辑（同时拦截 IPv4 和 IPv6 的本地回环）

		// 情况 A：纯 IPv6 本地回环地址，即 "::1"
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
			continue;
		}
		if (write(client_fd, &tinfo1, sizeof(tinfo1)) != sizeof(tinfo1))
		{
			printf("客户端断开连接\n");
			close(client_fd);
			continue; // 继续等待下一个客户端连接
		}
		// 打印连接信息（兼容 IPv4 和 IPv6 格式的打印）
		char ip_str[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &client_addr.sin6_addr, ip_str, sizeof(ip_str));
		printf("✓ 发送端: %s 已连接\n", ip_str);
		// 3. 创建接收线程
		pthread_create(&recv_thread, NULL, receive_thread, &client_fd);
	}
	if (server_socket >= 0)
	{
		close(server_socket);
	}
	printf("设备已销毁，程序退出\n");
	return 0;
}
