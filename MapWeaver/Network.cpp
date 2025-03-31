#include "Common.h"

#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include "Network.h"

using namespace std;

namespace internal
{
	constexpr static int networkConnectTimeout = 5; //连接超时限制（秒）
	constexpr static int networkLowSpeedLimit = 5000; //下载低速限制（字节/秒）
	constexpr static int networkLowSpeedTime = 10; // 低速下载时间限制（秒）
	constexpr static int networkRetryTimes = 3; //访问失败重试次数

	size_t WriteData(void* ptr, size_t size, size_t nmemb, void* stream)
	{
		if (!stream || !ptr || size == 0)
		{
			return 0;
		}
		const size_t realsize = size * nmemb;
		string* buffer = (string*)stream;
		buffer->append((const char*)ptr, realsize);
		return realsize;
	}

	int SSLCallback(CURL* curl, void* SSLCTX, void* userPtr)
	{
		if (!SSLCTX)
		{
			return 1;
		}
		SSL_CTX* ctx = static_cast<SSL_CTX*>(SSLCTX);
		if (!ctx)
		{
			return 1;
		}

		// 启用遗留重协商支持
		const uint64_t options = SSL_CTX_set_options(ctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
		if (!(options & SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION))
		{
			return 1;
		}
		return 0;
	}

	struct ImageData
	{
		char* buffer = nullptr;
		int size = 0;

		~ImageData()
		{
			if (buffer)
			{
				delete buffer;
				buffer = nullptr;
			}
		}
	};

	// 将下载的图片数据ptr写入ImageData类型的stream
	size_t WriteImageCallback(void* ptr, size_t size, size_t nmemb, void* stream)
	{
		if (!stream || !ptr || size == 0)
		{
			return 0;
		}

		const size_t realSize = size * nmemb;

		ImageData* imageData = static_cast<ImageData*>(stream);
		if (!imageData)
		{
			return realSize;
		}

		char* temp = (char*)realloc(imageData->buffer, imageData->size + realSize + 1);
		if (!temp)
		{
			return 0;
		}
		imageData->buffer = temp;
		memcpy(&(imageData->buffer[imageData->size]), ptr, realSize);
		imageData->size += realSize;
		imageData->buffer[imageData->size] = '\0'; // 确保缓冲区以 null 终止

		return realSize;
	}

    bool WriteImageToFile(CURL* curl, const ImageData& imageData, const string& filePath, string& receiveInfo)
    {
		if (!curl || !imageData.buffer)
		{
			receiveInfo = "Invalid parameters";
			return false;
		}

		if (imageData.size <= 0)
		{
			receiveInfo = "Empty image data";
			return false;
		}

		if (string(imageData.buffer)._Starts_with("<?xml")) // 接收到的实际上是XML, 跳过
		{
			return true;
		}

		curl_off_t offsize = 0;
		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &offsize);
		if (offsize >= 0 && offsize != imageData.size)
		{
			receiveInfo = "Error image data length";
			return false;
		}

		FILE* file = nullptr;
		if (fopen_s(&file, filePath.c_str(), "wb") != 0 || !file)
		{
			receiveInfo = "Failed to write file";
			return false;
		}
		fwrite(imageData.buffer, 1, imageData.size, file);
		fclose(file);
		return true;
    }

	bool IsNetworkError(CURLcode code)
	{
		switch (code)
		{
		case CURLE_COULDNT_CONNECT:
		case CURLE_OPERATION_TIMEDOUT:
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_SEND_ERROR:
		case CURLE_RECV_ERROR:
			return true;
		default:
			return false;
		}
	}
}


bool GetCapabilities(const string& url, string& content, string& receiveInfo, const string& requestJson, const string& proxyUrl, const string& proxyUserName, const string& proxyPassword)
{
	receiveInfo = "";
	content = "";

	if (url.empty())
	{
		receiveInfo = "Empty url";
		return false;
	}

	CURL* curl = curl_easy_init();
	if (!curl)
	{
		return false;
	}

	// 忽略证书检查
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);

	// 超时限制
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, internal::networkConnectTimeout);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, internal::networkLowSpeedTime);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, internal::networkLowSpeedLimit);

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::WriteData);
	curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, internal::SSLCallback);

	string receivedData = "";
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&receivedData);

	// 设置UTF-8编码
	curl_slist* listHeader = nullptr;
	listHeader = curl_slist_append(listHeader, "charset:utf-8");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, listHeader);

	// 重定向设置
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	// 谷歌服务需要json
	if (!requestJson.empty())
	{
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestJson.c_str());
	}

	// 执行请求
	CURLcode res = curl_easy_perform(curl);
	if (res == CURLE_OK)
	{
		// 处理HTTP请求
		LONG httpCode = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
		if (httpCode >= 200 && httpCode < 300)
		{
			content = receivedData;
			curl_easy_cleanup(curl);
			curl_slist_free_all(listHeader);
			return true;
		}
	}

	// 尝试使用代理重新请求
	if (!proxyUrl.empty())
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());

		// 设置代理的用户名和密码
		if (!proxyUserName.empty() && !proxyPassword.empty())
		{
			const string proxyAuth = proxyUserName + ":" + proxyPassword;
			curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, proxyAuth.c_str()); // 设置代理认证
		}
	}

	res = curl_easy_perform(curl);
	if (res != CURLE_OK)
	{
		switch (res)
		{
		case CURLE_UNSUPPORTED_PROTOCOL:
			receiveInfo = "Unsupported protocol";
			break;
		case CURLE_URL_MALFORMAT:
		case CURLE_COULDNT_RESOLVE_HOST:
			receiveInfo = "Wrong URL";
			break;
		case CURLE_COULDNT_CONNECT:
			receiveInfo = "Connect failed";
			break;
		case CURLE_OPERATION_TIMEDOUT:
			receiveInfo = "Operation timeout";
			break;
		case CURLE_SSL_CONNECT_ERROR:
			receiveInfo = "SSL connect error";
			break;
		default:
			receiveInfo = "Unknown error";
			break;
		}
		curl_easy_cleanup(curl);
		curl_slist_free_all(listHeader);
		content = "";
		return false;
	}

	// 处理HTTP请求
	LONG httpCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
	curl_easy_cleanup(curl);
	curl_slist_free_all(listHeader);
	if (httpCode < 200 || httpCode >= 300)
	{
		receiveInfo = "HTTP error";
		content = "";
		return false;
	}

	content = receivedData;
	return true;
}

bool DownloadImage(const string& url, const string& filePath, string& receiveInfo, const string& proxyUrl, const string& proxyUserName, const string& proxyPassword)
{
	receiveInfo = "";

	if (url.empty())
	{
		receiveInfo = "Empty url";
		return false;
	}

	CURL* curl = curl_easy_init();
	if (!curl)
	{
		return false;
	}

	// 忽略证书检查
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);

	// 超时限制
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, internal::networkConnectTimeout);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, internal::networkLowSpeedTime);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, internal::networkLowSpeedLimit);

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

	internal::ImageData imageData;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&imageData);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::WriteImageCallback);

	curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, internal::SSLCallback);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	// 执行请求
	CURLcode res = curl_easy_perform(curl);
	if (res == CURLE_OK)
	{
		// 处理HTTP请求
		LONG httpCode = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
		if (httpCode >= 200 && httpCode < 300)
		{
			if (!internal::WriteImageToFile(curl, imageData, filePath, receiveInfo))
			{
				curl_easy_cleanup(curl);
				return false;
			}
			curl_easy_cleanup(curl);
			return true;
		}
	}

	// 尝试使用代理重新请求
	if (!proxyUrl.empty())
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());

		// 设置代理的用户名和密码
		if (!proxyUserName.empty() && !proxyPassword.empty())
		{
			const string proxyAuth = proxyUserName + ":" + proxyPassword;
			curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, proxyAuth.c_str()); // 设置代理认证
		}
	}

	// 再次尝试请求
	res = curl_easy_perform(curl);
	if (res != CURLE_OK)
	{
		switch (res)
		{
		case CURLE_UNSUPPORTED_PROTOCOL:
			receiveInfo = "Unsupported protocol";
			break;
		case CURLE_URL_MALFORMAT:
		case CURLE_COULDNT_RESOLVE_HOST:
			receiveInfo = "Wrong URL";
			break;
		case CURLE_COULDNT_CONNECT:
			receiveInfo = "Connect failed";
			break;
		case CURLE_OPERATION_TIMEDOUT:
			receiveInfo = "Operation timeout";
			break;
		case CURLE_SSL_CONNECT_ERROR:
			receiveInfo = "SSL connect error";
			break;
		default:
			receiveInfo = "Unknown error";
			break;
		}
		curl_easy_cleanup(curl);
		return false;
	}

	// 处理HTTP请求
	LONG httpCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
	curl_easy_cleanup(curl);
	if (httpCode < 200 || httpCode >= 300)
	{
		receiveInfo = "HTTP error";
		return false;
	}

	if (!internal::WriteImageToFile(curl, imageData, filePath, receiveInfo))
	{
		return false;
	}
	return true;
}

bool DownloadAttempt(const string& url, const string& filePath, string& receiveInfo,
	const string& proxyUrl, const string& proxyUserName,
	const string& proxyPassword, bool useProxy, CURLcode& outCode)
{
	CURLM* multiHandle = curl_multi_init();
	if (!multiHandle)
	{
		receiveInfo = "Failed to initialize multiHandle";
		return false;
	}

	CURL* curl = curl_easy_init();
	if (!curl)
	{
		curl_multi_cleanup(multiHandle);
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, internal::networkConnectTimeout);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, internal::networkLowSpeedTime);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, internal::networkLowSpeedLimit);


	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

	internal::ImageData imageData;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &imageData);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, internal::WriteImageCallback);
	curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, internal::SSLCallback);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	if (useProxy && !proxyUrl.empty())
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());
		if (!proxyUserName.empty() || !proxyPassword.empty())
		{
			curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxyUserName.c_str());
			curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxyPassword.c_str());
		}
	}

	curl_multi_add_handle(multiHandle, curl);

	int still_running = 0;
	curl_multi_perform(multiHandle, &still_running);

	while (still_running)
	{
		int numfds;
		curl_multi_wait(multiHandle, nullptr, 0, 500, &numfds);
		curl_multi_perform(multiHandle, &still_running);
	}

	CURLMsg* msg = nullptr;
	int msgsLeft;
	bool success = true;
	outCode = CURLE_OK;
	while ((msg = curl_multi_info_read(multiHandle, &msgsLeft)))
	{
		if (msg->msg == CURLMSG_DONE)
		{
			if (msg->data.result != CURLE_OK)
			{
				success = false;
				outCode = msg->data.result;
				receiveInfo = "Download failed: " + string(curl_easy_strerror(outCode));
			}
		}
	}

	if (imageData.size <= 1024 && imageData.size > 0 && imageData.buffer)
	{
		const string imageStr = string(imageData.buffer);
		if (imageStr._Starts_with("<html") || imageStr._Starts_with("<!DOCTYPE"))
		{
			success = false;
			receiveInfo = "Network error";
			outCode = CURLE_RECV_ERROR;
		}
	}

	if (!success)
	{
		curl_multi_remove_handle(multiHandle, curl);
		curl_easy_cleanup(curl);
		curl_multi_cleanup(multiHandle);
		return false;
	}

	if (!internal::WriteImageToFile(curl, imageData, filePath, receiveInfo))
	{
		curl_multi_remove_handle(multiHandle, curl);
		curl_easy_cleanup(curl);
		curl_multi_cleanup(multiHandle);
		return false;
	}

	curl_multi_remove_handle(multiHandle, curl);
	curl_easy_cleanup(curl);
	curl_multi_cleanup(multiHandle);
	return true;
}

bool DownloadImageMultiThread(const string& url, const string& filePath, string& receiveInfo,
	const string& proxyUrl, const string& proxyUserName,
	const string& proxyPassword)
{
	if (url.empty())
	{
		receiveInfo = "Empty url";
		return false;
	}

	int retryCount = 0;
	while (retryCount <= 2)
	{
		receiveInfo.clear();
		// 第一次尝试：不使用代理
		CURLcode firstError = CURLE_OK;
		bool firstSuccess = DownloadAttempt(url, filePath, receiveInfo, "", "", "", false, firstError);
		if (firstSuccess)
		{
			return true;
		}

		// 如果是网络错误，尝试使用代理
		if (internal::IsNetworkError(firstError))
		{
			CURLcode secondError = CURLE_OK;
			if (DownloadAttempt(url, filePath, receiveInfo, proxyUrl, proxyUserName, proxyPassword, true, secondError))
			{
				return true;
			}
			retryCount++;
			continue;
		}
		retryCount++;
	}

	return false;
}

string EscapeString(const string& str)
{
	return curl_escape(str.c_str(), 0);
}

string GetStringMD5(const string& str)
{
	EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len;

	EVP_DigestInit_ex(mdctx, EVP_md5(), nullptr);
	EVP_DigestUpdate(mdctx, str.c_str(), str.size());
	EVP_DigestFinal_ex(mdctx, digest, &digest_len);
	EVP_MD_CTX_free(mdctx);

	// 将哈希转为十六进制字符串
	char mdString[33] = { 0 };
	for (unsigned int i = 0; i < digest_len; i++)
	{
		sprintf_s(mdString + (i * 2), sizeof(mdString) - (i * 2), "%02x", (unsigned int)digest[i]);
	}
	return string(mdString);
}
