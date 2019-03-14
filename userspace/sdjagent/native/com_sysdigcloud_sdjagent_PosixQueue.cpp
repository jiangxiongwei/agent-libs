#include "com_sysdigcloud_sdjagent_PosixQueue.h"
#include <sys/resource.h>
#include <mqueue.h>
#include "jni_utils.h"
#include <sys/time.h>
#include <errno.h>
#include <iostream>

using namespace std;

static const long MAX_MSGSIZE = 3 << 20; // 3 MiB
static const long MAX_QUEUES = 10;
static const long MAX_MSGS = 3;

/*
 * Class:     com_sysdigcloud_sdjagent_PosixQueue
 * Method:    openQueue
 * Signature: (Ljava/lang/String;II)I
 */
JNIEXPORT jint JNICALL Java_com_sysdigcloud_sdjagent_PosixQueue_openQueue
		(JNIEnv *env, jclass, jstring name , jint direction, jint maxmsgs)
{
	java_string queue_name(env, name);
	struct mq_attr queue_attrs = {0};
	int flags = O_CREAT;
	if(direction == 0)
	{
		// We need non_blocking mode only for send
		// on receive we use a timeout
		flags |= O_NONBLOCK | O_WRONLY;
		queue_attrs.mq_flags = O_NONBLOCK;
	} else {
		flags |= O_RDONLY;
	}
	queue_attrs.mq_maxmsg = maxmsgs;
	queue_attrs.mq_msgsize = MAX_MSGSIZE;
	queue_attrs.mq_curmsgs = 0;
	jint queue_d = mq_open(queue_name.c_str(), flags, S_IRWXU, &queue_attrs);
	if (queue_d > 0) {
		return queue_d;
	} else {
		return -errno;
	}
}

/*
 * Class:     com_sysdigcloud_sdjagent_PosixQueue
 * Method:    queueSend
 * Signature: (ILjava/lang/String;)Z
 */
JNIEXPORT jint JNICALL Java_com_sysdigcloud_sdjagent_PosixQueue_queueSend
		(JNIEnv *env, jclass, jint queue_d, jstring msg)
{
	java_string msg_data(env, msg);
	auto res = mq_send(queue_d, msg_data.c_str(), msg_data.size(), 0);
	if(res == 0)
	{
		return 0;
	}
	else
	{
		switch(errno)
		{
		case EAGAIN:
			return -1;
		case EMSGSIZE:
			return -2;
		default:
			return errno;
		}
	}
}

/*
 * Class:     com_sysdigcloud_sdjagent_PosixQueue
 * Method:    queueReceive
 * Signature: (IJ)Ljava/lang/String;
 */
JNIEXPORT jint JNICALL Java_com_sysdigcloud_sdjagent_PosixQueue_queueReceive
		(JNIEnv *env, jclass, jint queue_d, jbyteArray buffer, jlong timeout)
{
	struct timeval now;
	gettimeofday(&now, NULL);

	struct timespec ts = {0};
	ts.tv_sec = now.tv_sec + timeout;
	auto msgbuffer = env->GetByteArrayElements(buffer, NULL);
	auto msgbufferlen = env->GetArrayLength(buffer);
	auto res = mq_timedreceive(queue_d, (char*)msgbuffer, msgbufferlen, NULL, &ts);
	env->ReleaseByteArrayElements(buffer, msgbuffer, 0);
	if(res > 0)
	{
		return res;
	}
	else if (errno == ETIMEDOUT || errno == EINTR)
	{
		return -1;
	}
	else
	{
		return -errno;
	}
}

/*
 * Class:     com_sysdigcloud_sdjagent_PosixQueue
 * Method:    queueClose
 * Signature: (I)Z
 */
JNIEXPORT jboolean JNICALL Java_com_sysdigcloud_sdjagent_PosixQueue_queueClose
		(JNIEnv *, jclass, jint queue_d)
{
	return mq_close(queue_d) == 0;
}