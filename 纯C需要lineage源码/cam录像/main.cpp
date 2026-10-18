
#include <stdio.h>
#include <unistd.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayInfo.h>
// 引入 NDK 相机核心头文件
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>

// 引入 NDK 硬编码与媒体混流器头文件
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/Rect.h>
#include <binder/ProcessState.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

// 引入 AAudio 音频采集头文件
#include <aaudio/AAudio.h>
// 引入原有的 NDK 头文件
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

AMediaCodec *audio_encoder = nullptr;
AAudioStream *audio_stream = nullptr;
ssize_t audioTrackIndex = -1;
ssize_t videoTrackIndex = -1;

extern AMediaMuxer *muxer;
extern AMediaCodec *encoder;
extern int running;

bool initAudioEncoder(int sampleRate, int channelCount, int bitrate)
{
	AMediaFormat *audioFormat = AMediaFormat_new();
	AMediaFormat_setString(audioFormat, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
	AMediaFormat_setInt32(audioFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, sampleRate);
	AMediaFormat_setInt32(audioFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channelCount);
	AMediaFormat_setInt32(audioFormat, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);

	AMediaFormat_setInt32(audioFormat, "aac-profile", 2);

	audio_encoder = AMediaCodec_createEncoderByType("audio/mp4a-latm");
	if (!audio_encoder)
	{
		printf("错误: 无法创建音频 AAC 编码器\n");
		return false;
	}

	media_status_t status = AMediaCodec_configure(audio_encoder, audioFormat, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);

	AMediaFormat_delete(audioFormat);

	if (status != AMEDIA_OK)
	{
		printf("错误: 音频编码器配置失败!\n");
		return false;
	}

	AMediaCodec_start(audio_encoder);
	return true;
}

bool initAAudio(int sampleRate, int channelCount)
{
	AAudioStreamBuilder *builder = nullptr;
	aaudio_result_t result = AAudio_createStreamBuilder(&builder);
	if (result != AAUDIO_OK)
		return false;

	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
	AAudioStreamBuilder_setSampleRate(builder, sampleRate);
	AAudioStreamBuilder_setChannelCount(builder, channelCount);

	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

	AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);

	result = AAudioStreamBuilder_openStream(builder, &audio_stream);
	AAudioStreamBuilder_delete(builder);

	if (result != AAUDIO_OK)
	{
		printf("错误: 无法打开 AAudio 麦克风流! 错误码: %d\n", result);
		return false;
	}

	result = AAudioStream_requestStart(audio_stream);
	return result == AAUDIO_OK;
}

void *audio_polling_thread(void *)
{
	printf("[Audio Thread] Pipeline active. Capturing Microphone...\n");

	const int SAMPLES_PER_FRAME = 1024;
	int channelCount = 1;
	int bufferSizeSamples = SAMPLES_PER_FRAME * channelCount;
	int16_t *pcmBuffer = new int16_t[bufferSizeSamples];

	while (running)
	{
		int64_t timeoutNanoseconds = 20 * 1000000;
		aaudio_result_t framesRead = AAudioStream_read(audio_stream, pcmBuffer, SAMPLES_PER_FRAME, timeoutNanoseconds);
		if (framesRead > 0)
		{
			ssize_t inputBufIdx = AMediaCodec_dequeueInputBuffer(audio_encoder, 10000);
			if (inputBufIdx >= 0)
			{
				size_t bufSize;
				uint8_t *codecInputBuf = AMediaCodec_getInputBuffer(audio_encoder, inputBufIdx, &bufSize);

				int bytesToCopy = framesRead * channelCount * sizeof(int16_t);
				if (bytesToCopy > (int)bufSize)
					bytesToCopy = bufSize;

				memcpy(codecInputBuf, pcmBuffer, bytesToCopy);

				struct timespec ts;
				clock_gettime(1, &ts);
				int64_t currentPtsUs = (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;

				AMediaCodec_queueInputBuffer(audio_encoder, inputBufIdx, 0, bytesToCopy, currentPtsUs, 0);
			}
		}
	}

	delete[] pcmBuffer;
	printf("[Audio Thread] Exited safely.\n");
	return nullptr;
}

AMediaMuxer *muxer = 0;
AMediaCodec *encoder = 0;
int running = 1;
void sigal_handler(int)
{
	printf("sigal_handler\n");
	running = 0;
}

using namespace android;

ACameraDevice *g_cameraDevice = nullptr;
ACameraCaptureSession *g_captureSession = nullptr;
bool g_sessionActive = false;

void onDeviceDisconnected(void *, ACameraDevice *)
{
	printf("Camera Disconnected!\n");
}

void onDeviceError(void *, ACameraDevice *, int error)
{
	printf("Camera Error: %d\n", error);
}

ACameraDevice_StateCallbacks cameraDeviceCallbacks = {nullptr, onDeviceDisconnected, onDeviceError};

void onSessionActive(void *, ACameraCaptureSession *)
{
	g_sessionActive = true;
	printf("Capture Session Active!\n");
}

void onSessionReady(void *, ACameraCaptureSession *) {}

void onSessionClosed(void *, ACameraCaptureSession *) {}

ACameraCaptureSession_stateCallbacks sessionCallbacks = {nullptr, onSessionClosed, onSessionReady, onSessionActive};

void *sh_thread(void *arg)
{
	const char *shfile = (const char *)arg;
	char cmd[1255];
	sprintf(cmd, "sh %s", shfile);
	system(cmd);
	running = 0;
	return __null;
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

int main(int argc, char **argv)
{
	// 忽略 SIGPIPE 信号，防止客户端断开时程序闪退
	signal(SIGPIPE, SIG_IGN);
	int port = 10000;
	const char *sh_file = __null;
	const char *file = "/sdcard/native_record.mp4";
	int bitrate = 2000000;
	int width = 1920;
	int height = 1080;
	char bbuffer[255];
	int *Width = (int *)bbuffer;
	int *Height = (int *)(bbuffer + 4);
	*Height = height;
	*Width = width;
	int *sps_size = (int *)(bbuffer + 8);
	char *sps_buf = bbuffer + 12;
	int client_max = 10;
	int client_count = 0;
	int *client_arr = (int *)malloc(sizeof(int) * client_max);
	if (!client_arr)
	{
		printf("内存分配失败\n");
		return -1;
	}
	fd_set wd_set, rd_set;
	int maxfd = 0;
	if (argc > 1)
		file = argv[1];
	if (argc > 2)
		bitrate = atoi(argv[2]);
	if (argc > 3)
		sh_file = argv[3];

	if (bitrate < 100000)
		bitrate = 2000000;
	if (argc > 1 && strstr(argv[1], "-h"))
	{
		printf("%s [file.mp4] [bitrate: 默认:2000000] [file.sh 测试空间大小 电压。。 返回退出] [曝光: -2.0,2.0]\n", argv[0]);
		return 0;
	}
	int fd = open(file, 00000100 | 00000001 | 00001000, 0666);
	if (fd < 0)
	{
		printf("创建文件`%s`错误: %s (errno=%d)\n", file, strerror((*__errno())), (*__errno()));
		return fd;
	}
	if (sh_file)
	{
		pthread_t shThread;
		pthread_create(&shThread, nullptr, sh_thread, (void *)sh_file);
	}

	int audioSampleRate = 44100;
	int audioChannels = 1;
	int audioBitrate = 64000;

	printf("[Native Commandline Recorder] Booting... [Android 9/LineageOS 16]\n保存文件： %s\n", file);

	signal(2, sigal_handler);

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

	sp<SurfaceComposerClient> client = new SurfaceComposerClient();
	if (client == __null)
	{
		fprintf(stderr, "错误1\n");
		return 1;
	}

	client->initCheck();
	sp<IBinder> dtoken = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
	DisplayInfo dinfo;
	SurfaceComposerClient::getDisplayInfo(dtoken, &dinfo);

	// 如果成功获取到硬件分辨率，则更新宽高
	/*if (dinfo.w > 0 && dinfo.h > 0) {
	  width = dinfo.w;
	  height = dinfo.h;
	  }*/

	sp<SurfaceControl> surfaceControl = client->createSurface(String8("CameraNativePreview"), width / 2, height / 2, PIXEL_FORMAT_RGBA_8888, 0);
	if (surfaceControl == __null)
	{
		fprintf(stderr, "错误2\n");
		return 1;
	}

	// 核心补全 1：必须设置 setLayerStack(0) 绑定物理主屏，并将 alpha 改为 1.0f 避免在 stop 后的黑屏下隐形
	SurfaceComposerClient::Transaction t;
	t.setLayerStack(surfaceControl, 0)
		.setLayer(surfaceControl, 0x7FFFFFFF)
		.setPosition(surfaceControl, 0, 1440 - height / 2)
		.setAlpha(surfaceControl, 0.65f)
		.show(surfaceControl)
		.setColor(surfaceControl, {0.0f, 0.0f, 1.0f})
		.show(surfaceControl)
		.apply();

	sp<Surface> previewSurface = surfaceControl->getSurface();
	if (previewSurface == __null)
	{
		fprintf(stderr, "错误3\n");
		return 1;
	}

	ANativeWindow *previewWindow = previewSurface.get();
	if (previewWindow == __null)
	{
		fprintf(stderr, "错误4\n");
		return 1;
	}

	/*
	   ANativeWindow_Buffer buffer;
	   if (ANativeWindow_lock(previewWindow, &buffer, __null) == 0) {
	   uint32_t* bits = (uint32_t*)buffer.bits;
// 随便填充一个高亮显眼的颜色（例如：纯蓝色 0xFFFF0000 或纯红色 0xFF0000FF）
uint32_t testColor = 0xFFFF0000;
for (int y = 0; y < buffer.height; y++) {
for (int x = 0; x < buffer.width; x++) {
bits[x] = testColor;
}
bits += buffer.stride;
}
ANativeWindow_unlockAndPost(previewWindow);
fprintf(stdout, "[成功] 已成功向 Surface 提交了单帧测试颜色数据！\n");
} else {
fprintf(stderr, "错误: 锁定 ANativeWindow 失败\n");
}
	 */
	AMediaFormat *format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000100);

	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 2130708361);

	AMediaFormat_setInt32(format, "android.generic.index", 1);

	AMediaFormat_setInt32(format, "profile", 8);
	AMediaFormat_setInt32(format, "level", 65536);

	encoder = AMediaCodec_createEncoderByType("video/avc");
	int ret = AMediaCodec_configure(encoder, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	if (ret != AMEDIA_OK)
	{
		printf("错误 AMediaCodec_configure");
	}

	ANativeWindow *encoderWindow = nullptr;
	AMediaCodec_createInputSurface(encoder, &encoderWindow);

	muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);

	AMediaCodec_start(encoder);
	AMediaFormat_delete(format);
	if (!initAudioEncoder(audioSampleRate, audioChannels, audioBitrate) ||
		!initAAudio(audioSampleRate, audioChannels))
	{
		printf("音频链路组装失败，将退回纯视频录制模式！\n");
	}

	ACameraManager *cameraManager = ACameraManager_create();

	while (1)
	{
		camera_status_t status1 =
			ACameraManager_openCamera(cameraManager, "0", &cameraDeviceCallbacks, &g_cameraDevice);
		if (status1 == ACAMERA_OK)
			break;
		printf("openCamera打开失败 重试中\n");
		sleep(1);
	}

	ACaptureSessionOutputContainer *outputContainer = nullptr;
	ACaptureSessionOutputContainer_create(&outputContainer);

	ACaptureSessionOutput *previewOutput = nullptr;
	ACaptureSessionOutput_create(previewWindow, &previewOutput);
	ACaptureSessionOutputContainer_add(outputContainer, previewOutput);

	ACaptureSessionOutput *recordingOutput = nullptr;
	ACaptureSessionOutput_create(encoderWindow, &recordingOutput);
	ACaptureSessionOutputContainer_add(outputContainer, recordingOutput);

	ACameraDevice_createCaptureSession(g_cameraDevice, outputContainer, &sessionCallbacks, &g_captureSession);

	ACaptureRequest *captureRequest = nullptr;
	ACameraDevice_createCaptureRequest(g_cameraDevice, TEMPLATE_RECORD, &captureRequest);

	ACameraOutputTarget *previewTarget = nullptr;
	ACameraOutputTarget_create(previewWindow, &previewTarget);
	ACaptureRequest_addTarget(captureRequest, previewTarget);

	ACameraOutputTarget *recordingTarget = nullptr;
	ACameraOutputTarget_create(encoderWindow, &recordingTarget);
	/*
	//关闭自动曝光AE
	uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_OFF;
	ACaptureRequest_setEntry_u8(captureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
	//设置曝光时间
	int64_t exposureTime = 10000000; // 例如 10ms
	ACaptureRequest_setEntry_i64(captureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposureTime);
	//设置感光度
	int32_t sensitivity = 400; // ISO 值
	ACaptureRequest_setEntry_i32(captureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &sensitivity);
	 */
	/*自动曝光补偿（EV）如果想在自动曝光基础上调整亮度，直接设置曝光补偿即可，无需关闭 AE*/
	// 1. 获取设备支持的曝光补偿范围和步长
	ACameraMetadata *characteristics = 0;
	camera_status_t status12 = ACameraManager_getCameraCharacteristics(
		cameraManager,	 // 你的 ACameraManager 对象
		"0",			 // 相机 ID
		&characteristics // 输出参数，函数会把元数据写到这里
	);
	if (status12 != ACAMERA_OK)
	{
		printf("错误\n");
	}
	// printf("*************************%d %d**********\n",status12,ACAMERA_OK);
	if (status12 == ACAMERA_OK)
	{

		ACameraMetadata_const_entry rangeEntry, stepEntry;
		ACameraMetadata_getConstEntry(characteristics, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &rangeEntry);
		ACameraMetadata_getConstEntry(characteristics, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &stepEntry);

		int32_t minRange = rangeEntry.data.i32[0]; // 最小值
		int32_t maxRange = rangeEntry.data.i32[1]; // 最大值
		float step = (float)stepEntry.data.r[0].numerator / stepEntry.data.r[0].denominator;

		// 2. 假设你想设置 -2.0 EV
		float desiredEV = -1.3f;
		if (argc > 4)
			sscanf(argv[4], "%f", &desiredEV);
		int32_t compensationValue = (int32_t)(desiredEV / step); // 换算成步数

		// 3. 裁剪到合法范围内
		if (compensationValue < minRange)
			compensationValue = minRange;
		if (compensationValue > maxRange)
			compensationValue = maxRange;
		printf("min: %d max: %d  set: %d =====================\n", minRange, maxRange, compensationValue);
		// 4. 设置（此时数值一定在设备支持范围内）
		ACaptureRequest_setEntry_i32(captureRequest, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &compensationValue);
	}
	// 设置帧率
	int32_t fpsRange[2] = {30, 30};
	ACaptureRequest_setEntry_i32(captureRequest,
								 ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fpsRange);
	ACaptureRequest_addTarget(captureRequest, recordingTarget);

	ACameraCaptureSession_setRepeatingRequest(g_captureSession, nullptr, 1, &captureRequest, nullptr);

	printf("Camera pipeline coupled! Recording and Previewing for 10 seconds...\n");
	pthread_t audioThread;
	pthread_create(&audioThread, nullptr, audio_polling_thread, nullptr);

	AMediaCodecBufferInfo v_info;
	AMediaCodecBufferInfo a_info;

	printf("Dual-pipeline Muxer Loop pulling engaged! Monitoring Video & Audio...\n");
	uint8_t *nalu_data = 0;
	int client_fd = 0;
	int out_size; // 4bit
	size_t size;
	uint8_t *buffer = NULL;
	struct timeval tv = {0, 0};
	
	while (running)
	{

		ssize_t v_status = AMediaCodec_dequeueOutputBuffer(encoder, &v_info, 20000);
		if (v_status >= 0)
		{

			buffer = AMediaCodec_getOutputBuffer(encoder, v_status, &size);

			if (videoTrackIndex >= 0 && v_info.size > 0)
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
				ret = select(maxfd + 1, &rd_set, &wd_set, 0, &tv);
				if (ret < 0)
				{
					perror("错误 select()");
					break;
				}
				nalu_data = buffer + v_info.offset;
				if ((nalu_data[4] & 0x1f) == 5)
				{
				//	printf("I frame\n");
					if (FD_ISSET(server_fd, &rd_set))
					{
						// printf("有新客户端连接\n");
						client_fd = client_connect(server_fd);
						if (client_fd < 0)
						{
							continue;
						}

						out_size = 8;
						ret = sendd(client_fd, (char *)bbuffer, out_size);
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
						out_size = v_info.size;
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
							printf("发送SPS PPS %dx%d\n", *Width, *Height);
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
						ret = sendd(client_arr[i], (char *)nalu_data, v_info.size);
						if (ret != v_info.size)
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
				AMediaMuxer_writeSampleData(muxer, videoTrackIndex, buffer, &v_info);
			}
			AMediaCodec_releaseOutputBuffer(encoder, v_status, false);
		}
		if (v_status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
		{

			AMediaFormat *format = AMediaCodec_getOutputFormat(encoder);

			buffer = (uint8_t *)sps_buf;
			char *p = 0;
			// 从 format 中提取 csd-0 (SPS) 和 csd-1 (PPS)
			if (AMediaFormat_getBuffer(format, "csd-0", (void **)&p, &size) && p)
			{
				memcpy((void *)buffer, (void *)p, size);
				buffer += size;
				*sps_size = 0;
				*sps_size += size;
				printf("成功保存  SPS 配置帧，大小: %zu", size);
			}

			if (AMediaFormat_getBuffer(format, "csd-1", (void **)&p, &size) && p)
			{
				memcpy(buffer, p, size);
				*sps_size += size;
				printf("  成功保存 PPS 配置帧，大小: %zu  总大小%d\n", size, *sps_size);
			}
			if (videoTrackIndex < 0)
			{
				videoTrackIndex = AMediaMuxer_addTrack(muxer, format);
				AMediaFormat_delete(format);
				printf("[Muxer] Video Track Added via FORMAT_CHANGED.\n");
				if(audioTrackIndex >= 0){
					AMediaMuxer_start(muxer);
					printf("[Muxer]  Muxer_start video.\n");
				}
			}
		}

		ssize_t a_status = AMediaCodec_dequeueOutputBuffer(audio_encoder, &a_info, 20000);
		if (a_status >= 0)
		{
			size_t size;
			uint8_t *buffer = AMediaCodec_getOutputBuffer(audio_encoder, a_status, &size);
			if (audioTrackIndex >= 0 && a_info.size > 0)
			{
				AMediaMuxer_writeSampleData(muxer, audioTrackIndex, buffer, &a_info);
			}
			AMediaCodec_releaseOutputBuffer(audio_encoder, a_status, false);
		}
		else if (a_status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
		{

			AMediaFormat *format = AMediaCodec_getOutputFormat(audio_encoder);
			if (audioTrackIndex < 0)
			{
				audioTrackIndex = AMediaMuxer_addTrack(muxer, format);
				AMediaFormat_delete(format);
				printf("[Muxer] Audio Track Added via FORMAT_CHANGED.\n");
				if(videoTrackIndex >= 0){
					AMediaMuxer_start(muxer);
					printf("[Muxer] Muxer_start audio.\n");
				}
			}
			printf("[Muxer] Both Tracks Armed! Muxer started safely.\n");
		}

		if (running == 0)
			break;
	}
	printf("Stopping recording... Cleaning up native structures.\n");

	pthread_join(audioThread, nullptr);

	if (audio_stream)
	{
		AAudioStream_requestStop(audio_stream);
		AAudioStream_close(audio_stream);
	}
	if (audio_encoder)
	{
		AMediaCodec_stop(audio_encoder);
		AMediaCodec_delete(audio_encoder);
	}

	if (g_captureSession)
	{
		ACameraCaptureSession_stopRepeating(g_captureSession);
		ACameraCaptureSession_close(g_captureSession);
	}
	if (g_cameraDevice)
	{
		ACameraDevice_close(g_cameraDevice);
	}
	if (encoder)
	{
		AMediaCodec_stop(encoder);
		AMediaCodec_delete(encoder);
	}

	if (muxer)
	{

		media_status_t close_status = AMediaMuxer_stop(muxer);
		if (close_status != AMEDIA_OK)
		{
			printf("错误AMediaMuxer_stop ret=%d\n", close_status);
		}
		else
		{
			printf("AMediaMuxer_stop 成功！音视频双轨 MP4 完美闭合。\n");
		}

		AMediaMuxer_delete(muxer);
	}

	close(fd);
	printf("保存文件到%s\n", file);
	return 0;
}
