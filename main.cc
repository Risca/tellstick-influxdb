#include <telldus-core.h>

#include <curl/curl.h>
#include <errno.h>
#include <iostream>
#include <semaphore.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <vector>

#define DATA_LENGTH 20

static bool running = true;
static sem_t semaphore;

struct Value {
	int id;
	float temp;
	int timestamp;
};

typedef std::vector<Value> ValueList;
struct Context {
	std::set<int> watchedIds;
	ValueList values;
	pthread_mutex_t valueMutex;
};

class MutexLocker
{
public:
	MutexLocker(pthread_mutex_t* mtx) : m_Mutex(mtx) { pthread_mutex_lock(m_Mutex); }
	~MutexLocker() { pthread_mutex_unlock(m_Mutex); }

private:
	pthread_mutex_t* m_Mutex;
};

void stop(int signum)
{
	running = false;
	sem_post(&semaphore);
}

void setupSigHandler(void) {
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = stop;
	sigaction(SIGTERM, &act, NULL);
}

void SensorCb(const char *protocol,
              const char *model,
              int id,
              int dataType,
              const char *value,
              int timestamp,
              int callbackId,
              void *context)
{
	Context* ctx = (Context*)context;

	if (ctx->watchedIds.count(id) == 0) {
		syslog(LOG_DEBUG, "Ignoring measurement from sensor id %d", id);
		return;
	}

	MutexLocker lock(&ctx->valueMutex);
	char* endptr = NULL;
	Value v;
	v.temp = strtof(value, &endptr);
	if (errno == ERANGE || value == endptr) {
		// overflow or no conversion done
		return;
	}
	v.id = id;
	v.timestamp = timestamp;
	ctx->values.push_back(v);

	pthread_mutex_unlock(&ctx->valueMutex);

	if (sem_post(&semaphore) < 0) {
		syslog(LOG_ERR, "Failed to post semaphore: %m");
	}
}

std::string GetInfluxLine(const Value& v)
{
	std::stringstream ss;
	ss << "temperature";
	ss << ",location=Jacuzzi";
	ss << ",serial=" << v.id;
	ss << ",source=Tellstick";
	ss << ",type=Pool\\ thermometer";
	ss << " value=" << v.temp;
	ss << " " << v.timestamp << "000000000";
	ss << std::endl;
	return ss.str();
}

int PostInfluxData(const std::string& values)
{
	CURL* curl;
	CURLcode rc;

	curl = curl_easy_init();
	if (!curl) {
		syslog(LOG_ERR, "Failed to initialize curl_easy: %s", curl_easy_strerror(rc));
		return -1;
	}

	struct curl_slist* headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
	if (!headers) {
		syslog(LOG_ERR, "Failed to create Content-Type header");
		return -1;
	}

	rc = curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8086/write?db=mydb");
	if (rc != CURLE_OK) {
		syslog(LOG_ERR, "Failed to set CURLOPT_URL: %s", curl_easy_strerror(rc));
		return -1;
	}

	rc = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, values.data());
	if (rc != CURLE_OK) {
		syslog(LOG_ERR, "Failed to set CURLOPT_POSTFIELDS: %s", curl_easy_strerror(rc));
		return -1;
	}

	rc = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, values.size());
	if (rc != CURLE_OK) {
		syslog(LOG_ERR, "Failed to set CURLOPT_POSTFIELDSIZE: %s", curl_easy_strerror(rc));
		return -1;
	}

	rc = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	if (rc != CURLE_OK) {
		syslog(LOG_ERR, "Failed to set CURLOPT_HTTPHEADER: %s", curl_easy_strerror(rc));
		return -1;
	}

	rc = curl_easy_perform(curl);
	if (rc != CURLE_OK) {
		syslog(LOG_ERR, "Failed to POST: %s", curl_easy_strerror(rc));
		return -1;
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return 0;
}

std::string GetIdList(const std::set<int>& set)
{
	std::stringstream ss;
	const char* separator = "";
	for (std::set<int>::const_iterator it = set.begin(); it != set.end(); ++it) {
		ss << separator << *it;
		separator = " ";
	}
	return ss.str();
}

std::string GetIdList(const ValueList& list)
{
	std::stringstream ss;
	const char* separator = "";
	for (ValueList::const_iterator it = list.begin(); it != list.end(); ++it) {
		ss << separator << it->id;
		separator = " ";
	}
	return ss.str();
}

int main(int argc, char* argv[])
{
	int rc, callbackId;
	Context context;
	Context* ctx = &context;

	if (argc < 2) {
		std::cout << basename(argv[0]) << " <sensor ids>" << std::endl;
		return -1;
	}

	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
		std::cerr << "Failed to initialize curl" << std::endl;
		return -1;
	}

	setupSigHandler();

	rc = sem_init(&semaphore, 0, 0);
	if (rc < 0) {
		perror("Failed to create semaphore");
		return -1;
	}

	rc = pthread_mutex_init(&ctx->valueMutex, NULL);
	if (rc != 0) {
		std::cerr << "Failed to initialize mutex: " << strerror(rc) << std::endl;
		return -1;
	}

	for (int i = 1; i < argc; ++i) {
		char* endptr = NULL;
		int id = strtol(argv[i], &endptr, 0);
		if (errno != 0) {
			perror("Invalid id");
			return -1;
		}
		ctx->watchedIds.insert(id);
	}

	rc = daemon(0, 0);
	if (rc < 0) {
		perror("Failed to daemonize");
		return -1;
	}

	openlog(NULL, LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "Init done, listening for sensor%s: %s",
	                 ctx->watchedIds.size() == 1 ? "" : "s",
			 GetIdList(ctx->watchedIds).c_str());
	callbackId = tdRegisterSensorEvent(SensorCb, ctx);
	while (running) {
		rc = sem_wait(&semaphore);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			syslog(LOG_ERR, "Failed to wait on semaphore: %m");
			break;
		}

		pthread_mutex_lock(&ctx->valueMutex);
		ValueList &values = ctx->values;
		const int valueCount = values.size();
		const std::string idList = GetIdList(values);
		ValueList::iterator it = values.begin();
		std::stringstream ss;
		while (it != values.end()) {
			ss << GetInfluxLine(*it);
			it = values.erase(it);
		}
		pthread_mutex_unlock(&ctx->valueMutex);

		rc = PostInfluxData(ss.str());
		if (rc == 0) {
			syslog(LOG_NOTICE, "Posted %d values to influx db from sensor%s: %s",
			                   valueCount, valueCount == 1 ? "" : "s",
			                   idList.c_str());
		}
	}
	
	syslog(LOG_NOTICE, "Shutting down");

	rc = sem_destroy(&semaphore);
	if (rc < 0) {
		syslog(LOG_WARNING, "Failed to destroy semaphore: %m");
	}

	rc = pthread_mutex_destroy(&ctx->valueMutex);
	if (rc < 0) {
		syslog(LOG_WARNING, "Failed to destroy mutex: %m");
	}

	rc = tdUnregisterCallback(callbackId);
	if (rc != TELLSTICK_SUCCESS) {
		syslog(LOG_WARNING, "Failed to unregister callback: %s\n", tdGetErrorString(rc));
	}

	curl_global_cleanup();

	syslog(LOG_NOTICE, "Exit");
	closelog();
	return 0;
}

