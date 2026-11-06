/*
	Copyright 2022 flyinghead

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Flycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#import <Foundation/Foundation.h>
#include "oslib/http_client.h"
#include "stdclass.h"

// gdxsv: delegate of the asynchronous NSURLSession request in http::get
@interface NSURLSessionHandler:NSObject <NSURLSessionDataDelegate, NSURLSessionDelegate, NSURLSessionTaskDelegate>
- (void)setSemaphore:(dispatch_semaphore_t)sema;
- (void)setContent:(std::vector<u8>&)content;
@property(nonatomic, retain) NSHTTPURLResponse* httpResponse;
@property(nonatomic, retain) NSError* httpError;
@end
@implementation NSURLSessionHandler
{
	dispatch_semaphore_t _sema;
	NSHTTPURLResponse* _httpResponse;
	NSError* _httpError;
	std::vector<u8>* _content;
}

- (void)setSemaphore:(dispatch_semaphore_t)sema { _sema = sema; }

- (void)setContent:(std::vector<u8>&)content { _content = &content; }

@synthesize httpResponse = _httpResponse;
@synthesize httpError = _httpError;

#if !__has_feature(objc_arc)
- (void)dealloc {
	[_httpResponse release];
	[_httpError release];
	[super dealloc];
}
#endif
	
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition disposition))completionHandler {
	completionHandler(NSURLSessionResponseAllow);
	self.httpResponse = (NSHTTPURLResponse *)response;
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveData:(NSData *)data {
	_content->insert(_content->end(), (const u8 *)[data bytes], (const u8 *)[data bytes] + [data length]);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
didCompleteWithError:(nullable NSError *)error {
	self.httpError = error;
	dispatch_semaphore_signal(_sema);
}

@end

namespace http {

int get(const std::string& url, std::vector<u8>& content, const Headers *reqHeaders, Headers *respHeaders)
{
	NSString *nsurl = [NSString stringWithCString:url.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
    NSString *userAgent = [NSString stringWithCString:getUserAgent().c_str()
                                             encoding:[NSString defaultCStringEncoding]];
	NSMutableURLRequest *urlRequest = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:nsurl]];
	[urlRequest setValue:userAgent forHTTPHeaderField:@"User-Agent"];
    if (reqHeaders != nullptr)
    {
        for (const auto& [ name, value ] : *reqHeaders)
        {
            NSString *nsname = [NSString stringWithCString:name.c_str()
                                                  encoding:[NSString defaultCStringEncoding]];
            NSString *nsvalue = [NSString stringWithCString:value.c_str()
                                                   encoding:[NSString defaultCStringEncoding]];
            [urlRequest setValue:nsvalue forHTTPHeaderField:nsname];
        }
    }

	// gdxsv: callbacks run independently of the main thread so shutdown can wait for HTTP workers.
	NSURLSessionHandler* handler = [[NSURLSessionHandler alloc] init];
	content.clear();
	[handler setContent:content];
	NSURLSessionConfiguration *defaultConfigObject = [NSURLSessionConfiguration defaultSessionConfiguration];
	NSURLSession *defaultSession = [NSURLSession sessionWithConfiguration: defaultConfigObject delegate: handler delegateQueue: nil];
	NSURLSessionDataTask *dataTask = [defaultSession dataTaskWithRequest: urlRequest];
	
	dispatch_semaphore_t sema = dispatch_semaphore_create(0);
	[handler setSemaphore:sema];
	
	[dataTask resume];
	
	if (![NSThread isMainThread]) {
		dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
	} else {
		while (dispatch_semaphore_wait(sema, DISPATCH_TIME_NOW)) {
			[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0]];
		}
	}
	
	const int status = [handler httpError] != nil || [handler httpResponse] == nil
			? 500 : (int)[[handler httpResponse] statusCode];

	if (respHeaders != nullptr && [handler httpError] == nil)
	{
		NSDictionary *headers = [handler httpResponse].allHeaderFields;
		NSEnumerator *enumerator = [headers keyEnumerator];
		id key;
		while ((key = [enumerator nextObject]))
		{
			std::string strkey([key UTF8String]);
			string_tolower(strkey);
			std::string strvalue([[headers objectForKey:key] UTF8String]);
			respHeaders->emplace_back(strkey, strvalue);
		}
	}
	
	// gdxsv: content is complete; release the session and its retained callback results.
	[defaultSession finishTasksAndInvalidate];
#if !__has_feature(objc_arc)
	[handler release];
	dispatch_release(sema);
#endif
	return status;
}

int post(const std::string& url, const char *payload, const char *contentType, std::vector<u8>& reply)
{
	NSString *nsurl = [NSString stringWithCString:url.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
    NSString *userAgent = [NSString stringWithCString:getUserAgent().c_str()
                                             encoding:[NSString defaultCStringEncoding]];
	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:nsurl]];
	[request setHTTPMethod:@"POST"];
	[request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
	[request setHTTPShouldHandleCookies:NO];
	
	size_t payloadSize = strlen(payload);
	[request setHTTPBody:[NSData dataWithBytes:payload length:payloadSize]];
	NSString *postLength = [NSString stringWithFormat:@"%ld", (unsigned long)payloadSize];
    [request setValue:postLength forHTTPHeaderField:@"Content-Length"];
    NSString *nscontentType = contentType != nullptr ? [NSString stringWithCString:contentType 
                                         encoding:[NSString defaultCStringEncoding]]
    	: @"application/x-www-form-urlencoded";
    [request setValue:nscontentType forHTTPHeaderField:@"Content-Type"];
	[request setValue:userAgent forHTTPHeaderField:@"User-Agent"];
    
	NSURLResponse *response = nil;
	NSError *error = nil;
	NSData *data = [NSURLConnection sendSynchronousRequest:request
                          returningResponse:&response
                                      error:&error];
	if (error != nil)
		return 500;

	NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response; 
		
	reply.clear();
	reply.insert(reply.begin(), (const u8 *)[data bytes], (const u8 *)[data bytes] + [data length]);

	return [httpResponse statusCode];
}

int post(const std::string& url, const std::vector<PostField>& fields)
{
	NSString *nsurl = [NSString stringWithCString:url.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
    NSString *userAgent = [NSString stringWithCString:getUserAgent().c_str()
                                             encoding:[NSString defaultCStringEncoding]];
	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:nsurl]];
	[request setHTTPMethod:@"POST"];
	[request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
	[request setHTTPShouldHandleCookies:NO];

	NSString *boundary = @"----flycast-boundary-7192397596";
	NSString *contentType = [NSString stringWithFormat:@"multipart/form-data; boundary=%@", boundary];
	[request setValue:contentType forHTTPHeaderField: @"Content-Type"];
	[request setValue:userAgent forHTTPHeaderField:@"User-Agent"];
	
	NSMutableData *body = [NSMutableData data];
	for (const PostField& field : fields)
	{
        NSString *value = [NSString stringWithCString:field.value.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
		[body appendData:[[NSString stringWithFormat:@"--%@\r\n", boundary] dataUsingEncoding:NSUTF8StringEncoding]];
		[body appendData:[[NSString stringWithFormat:@"Content-Disposition: form-data; name=\"%@\"", [NSString stringWithCString:field.name.c_str() 
                                         encoding:[NSString defaultCStringEncoding]]] dataUsingEncoding:NSUTF8StringEncoding]];
        if (!field.contentType.empty())
        {
	        [body appendData:[[NSString stringWithFormat:@"; filename=\"%@\"\r\n", value] dataUsingEncoding:NSUTF8StringEncoding]];
	        [body appendData:[[NSString stringWithFormat:@"Content-Type: %@", [NSString stringWithCString:field.contentType.c_str() 
                                         encoding:[NSString defaultCStringEncoding]]] dataUsingEncoding:NSUTF8StringEncoding]];
        }
        [body appendData:[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding]];
        
        if (field.contentType.empty())
        {
	        [body appendData:[value dataUsingEncoding:NSUTF8StringEncoding]];
        }
        else
        {
        	NSError* error = nil;
			NSData *filedata = [NSData dataWithContentsOfFile:value options:0 error:&error];
			if (error != nil)
				return 500;
	        [body appendData:filedata];
        }
        [body appendData:[@"\r\n" dataUsingEncoding:NSUTF8StringEncoding]];
	}
	[body appendData:[[NSString stringWithFormat:@"--%@--\r\n", boundary] dataUsingEncoding:NSUTF8StringEncoding]];
	
	[request setHTTPBody:body];
	NSString *postLength = [NSString stringWithFormat:@"%ld", (unsigned long)[body length]];
    [request setValue:postLength forHTTPHeaderField:@"Content-Length"];
    
	NSURLResponse *response = nil;
	NSError *error = nil;
	[NSURLConnection sendSynchronousRequest:request
                          returningResponse:&response
                                      error:&error];
	if (error != nil)
		return 500;

	NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response; 
	return [httpResponse statusCode];
}

void init() {
}

void term() {
}

}
