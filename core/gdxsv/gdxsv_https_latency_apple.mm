#import <Foundation/Foundation.h>

#include <chrono>

#include "gdxsv_https_latency.h"
#include "oslib/http_client.h"

namespace {

struct HeadResponse {
	int status = 0;
	std::string error;
};

static HeadResponse performHead(NSURLSession *session, NSURLRequest *request)
{
	dispatch_semaphore_t sema = dispatch_semaphore_create(0);
	__block NSHTTPURLResponse *http_response = nil;
	__block NSError *http_error = nil;

	NSURLSessionDataTask *task = [session dataTaskWithRequest:request
		completionHandler:^(NSData *, NSURLResponse *response, NSError *error) {
			if ([response isKindOfClass:[NSHTTPURLResponse class]])
				http_response = [(NSHTTPURLResponse *)response retain];
			if (error != nil)
				http_error = [error retain];
			dispatch_semaphore_signal(sema);
		}];
	[task resume];

	dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

	HeadResponse result;
	if (http_error != nil)
	{
		result.error = [[http_error localizedDescription] UTF8String];
		[http_error release];
	}
	if (http_response != nil)
	{
		result.status = (int)[http_response statusCode];
		[http_response release];
	}

	return result;
}

} // namespace

GdxsvHttpsLatencyResult measureGdxsvHttpsLatency(const std::string& host, const std::string& path, int attempts)
{
	GdxsvHttpsLatencyResult result;

	@autoreleasepool {
		NSString *url = [NSString stringWithFormat:@"https://%s%s", host.c_str(), path.c_str()];
		NSString *user_agent = [NSString stringWithCString:http::getUserAgent().c_str()
			encoding:[NSString defaultCStringEncoding]];

		NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
		[request setHTTPMethod:@"HEAD"];
		[request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
		[request setHTTPShouldHandleCookies:NO];
		[request setValue:user_agent forHTTPHeaderField:@"User-Agent"];

		NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration defaultSessionConfiguration];
		configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		configuration.HTTPShouldSetCookies = NO;
		NSURLSession *session = [[NSURLSession sessionWithConfiguration:configuration] retain];

		HeadResponse response = performHead(session, request);
		if (!gdxsv_https_latency_answered(response.status))
		{
			result.status = response.status;
			result.error = response.error.empty() ? "warmup failed" : response.error;
			[session finishTasksAndInvalidate];
			[session release];
			return result;
		}

		for (int i = 0; i < attempts; i++)
		{
			auto t1 = std::chrono::high_resolution_clock::now();
			response = performHead(session, request);
			auto t2 = std::chrono::high_resolution_clock::now();

			if (!gdxsv_https_latency_answered(response.status))
			{
				result.status = response.status;
				result.error = response.error.empty() ? "request failed" : response.error;
				[session finishTasksAndInvalidate];
				[session release];
				return result;
			}

			const int rtt = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
			result.attempts_ms.push_back(rtt);
			if (result.min_ms == -1 || rtt < result.min_ms)
				result.min_ms = rtt;
			result.status = response.status;
		}

		[session finishTasksAndInvalidate];
		[session release];
	}

	result.ok = result.min_ms >= 0;
	return result;
}
