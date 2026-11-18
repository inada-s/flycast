#import <Foundation/Foundation.h>

#include "gdxsv_https_latency.h"
#include "oslib/http_client.h"

namespace {

constexpr int gdxsv_https_latency_timeout_seconds = 10;

struct GdxsvHttpsLatencyResponse
{
	int status = 0;
	int first_byte_ms = -1;
	std::string error;
};

} // namespace

@interface GdxsvHttpsLatencyDelegate : NSObject <NSURLSessionTaskDelegate>
{
@public
	GdxsvHttpsLatencyResponse response;
	dispatch_semaphore_t completed;
}
@end

@implementation GdxsvHttpsLatencyDelegate

- (instancetype)init
{
	self = [super init];
	if (self != nil)
		completed = dispatch_semaphore_create(0);
	return self;
}

- (void)dealloc
{
	dispatch_release(completed);
	[super dealloc];
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
	didFinishCollectingMetrics:(NSURLSessionTaskMetrics *)metrics
{
	NSURLSessionTaskTransactionMetrics *first = metrics.transactionMetrics.firstObject;
	NSURLSessionTaskTransactionMetrics *last = metrics.transactionMetrics.lastObject;
	if (first.fetchStartDate == nil || last.responseStartDate == nil
			|| last.resourceFetchType != NSURLSessionTaskMetricsResourceFetchTypeNetworkLoad)
		return;

	// HEAD response callbacks can wait for stream completion; this timestamp records the first byte.
	const double milliseconds = [last.responseStartDate timeIntervalSinceDate:first.fetchStartDate] * 1000.0;
	response.first_byte_ms = static_cast<int>(milliseconds);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
	if ([task.response isKindOfClass:[NSHTTPURLResponse class]])
		response.status = (int)[(NSHTTPURLResponse *)task.response statusCode];
	if (error != nil)
		response.error = [[error localizedDescription] UTF8String];
	else if (response.first_byte_ms < 0)
		response.error = "response timing unavailable";
	dispatch_semaphore_signal(completed);
}

@end

namespace {

static GdxsvHttpsLatencyResponse gdxsv_perform_head(NSURLSession *session, NSURLRequest *request,
	GdxsvHttpsLatencyDelegate *delegate)
{
	delegate->response = {};
	NSURLSessionDataTask *task = [session dataTaskWithRequest:request];
	[task resume];

	const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, (gdxsv_https_latency_timeout_seconds + 5) * NSEC_PER_SEC);
	if (dispatch_semaphore_wait(delegate->completed, deadline) != 0)
	{
		[task cancel];
		GdxsvHttpsLatencyResponse response;
		response.error = "response timed out";
		return response;
	}
	return delegate->response;
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
		configuration.URLCache = nil;
		configuration.timeoutIntervalForRequest = gdxsv_https_latency_timeout_seconds;
		configuration.timeoutIntervalForResource = gdxsv_https_latency_timeout_seconds;
		GdxsvHttpsLatencyDelegate *delegate = [[GdxsvHttpsLatencyDelegate alloc] init];
		NSOperationQueue *queue = [[NSOperationQueue alloc] init];
		queue.maxConcurrentOperationCount = 1;
		NSURLSession *session = [[NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue] retain];
		[queue release];

		GdxsvHttpsLatencyResponse response = gdxsv_perform_head(session, request, delegate);
		if (!response.error.empty() || !gdxsv_https_latency_answered(response.status))
		{
			result.status = response.status;
			result.error = response.error.empty() ? "warmup failed" : response.error;
			[session invalidateAndCancel];
			[session release];
			[delegate release];
			return result;
		}

		for (int i = 0; i < attempts; i++)
		{
			response = gdxsv_perform_head(session, request, delegate);

			if (!response.error.empty() || !gdxsv_https_latency_answered(response.status))
			{
				result.status = response.status;
				result.error = response.error.empty() ? "request failed" : response.error;
				[session invalidateAndCancel];
				[session release];
				[delegate release];
				return result;
			}

			const int rtt = response.first_byte_ms;
			result.attempts_ms.push_back(rtt);
			if (result.min_ms == -1 || rtt < result.min_ms)
				result.min_ms = rtt;
			result.status = response.status;
		}

		[session finishTasksAndInvalidate];
		[session release];
		[delegate release];
	}

	result.ok = result.min_ms >= 0;
	return result;
}
