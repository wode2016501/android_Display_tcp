int read_all(int fd, void *buf, int size)
{
	int n = size;
	int ret = 0;
	while (n > 0)
	{
		ret = read(fd, buf, n);
		if (ret < 1)
		{
			return -1;
		}
		n -= ret;
		buf += ret;
	}
	return size;
}
int main()
{
	int w = 0;
	int h = 0;
	int ret = 0;
	int size = 0;
	char buf[5 * 1024 * 1024];
	ret = read_all(0, &w, sizeof(w));
	if (ret != sizeof(w))
	{
		return -1;
	}
	ret = read_all(0, &h, sizeof(h));
	if (ret != sizeof(h))
	{
		return -1;
	}
	//printf("%dx%d\n", w, h);
	while (ret > 0)
	{
		ret = read_all(0, &size, sizeof(size));
		if (ret !=sizeof(size))
			return 0;
		if (size < 0)
			return -1;
		if (size > 0)
		{
		//	printf("size: %d\n",size);
			ret = read_all(0, buf, size);
			if (ret != size)
			{
				return -1;
			}
			ret = write(1, buf, size);
			if (ret != size)
				return -1;
		}
	}
}