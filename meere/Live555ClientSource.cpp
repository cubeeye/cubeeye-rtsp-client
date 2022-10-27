/*
 * Live555ClientSource.cpp
 *
 *  Created on: 2022. 10. 4.
 *      Author: erato
 */

#include <cassert>
#include <cstdarg>
#include <vector>
#include <string>
#include "Live555ClientSource.h"
#include "BasicUsageEnvironment.hh"
#if defined(HAVE_EPOLL_SCHEDULER)
#include "EpollTaskScheduler.hh"
#endif //HAVE_EPOLL_SCHEDULER

// Implementation of "RTSPClientSink":
// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define SINK_RECEIVE_BUFFER_SIZE 16 * 1024 * 1024

// by default, print verbose output from each "RTSPClient"
#ifdef DEBUG
#define RTSP_CLIENT_VERBOSITY_LEVEL 1
#else //!DEBUG
#define RTSP_CLIENT_VERBOSITY_LEVEL 0
#endif //DEBUG

// By default, we request that the server stream its data using RTP/UDP.
// If, instead, you want to request that the server stream via RTP-over-TCP, change the following to True:
#define REQUEST_STREAMING_OVER_TCP true

inline std::string make_log_message(const char* format, ...)
{
	va_list args1;
	va_start(args1, format);
	va_list args2;
	va_copy(args2, args1);
	std::vector<char> buf(1 + std::vsnprintf(NULL, 0, format, args1));
	va_end(args1);
	std::vsnprintf(buf.data(), buf.size(), format, args2);
	va_end(args2);
	return std::string(buf.data());
}


// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

// RTSPClientSink
Live555ClientSource::RTSPClientSink::RTSPClientSink(UsageEnvironment& env, MediaSubsession& subsession, Live555ClientSource* source)
 : MediaSink(env)
 , mMediaSubsession(subsession)
 , mUptrReceivedBuffer(new uint8_t[SINK_RECEIVE_BUFFER_SIZE])
 , mClientSource(source)
{
	assert(mUptrReceivedBuffer);
}

Live555ClientSource::RTSPClientSink::~RTSPClientSink()
{
	mClientSource = nullptr;
	mUptrReceivedBuffer.reset();
}

Boolean Live555ClientSource::RTSPClientSink::continuePlaying()
{
	if (nullptr != mClientSource) {
		std::lock_guard<std::mutex> _lock(mClientSource->mSinkLock);
		if (nullptr != MediaSink::fSource) {
			/*
			 * Request the next frame of data from our input source.
			 * "afterGettingFrame()" will get called later, when it arrives:
			 */
			MediaSink::fSource->getNextFrame(&mUptrReceivedBuffer[0], SINK_RECEIVE_BUFFER_SIZE, \
					afterGettingFrame, this, MediaSink::onSourceClosure, this);
			return True;
		}
	}

	return False;
}

void Live555ClientSource::RTSPClientSink::afterGettingFrame(void* clientData, unsigned frameSize, \
		unsigned numTruncatedBytes, struct timeval presentationTime, unsigned durationInMicroseconds)
{
	auto _sink = static_cast<RTSPClientSink*>(clientData);
	if (nullptr != _sink && nullptr != _sink->mClientSource) {
		Live555ClientSource::Listener* _listener = _sink->mClientSource->mListener;
		if (nullptr != _listener) {
			MediaSubsession* _subsession = &_sink->mMediaSubsession;
			uint16_t _width = _subsession->videoWidth() & 0xFFFF;
			uint16_t _height = _subsession->videoHeight() & 0xFFFF;
			uint64_t _timestamp = presentationTime.tv_sec * 1000000 + presentationTime.tv_usec;
			std::string _codec(_subsession->codecName());
			std::string _path(_subsession->controlPath());

			auto _rtp_source = _subsession->rtpSource();
			if (nullptr != _rtp_source && !_rtp_source->hasBeenSynchronizedUsingRTCP()) {
				// only H.264/265 packet!
				auto _npt = _subsession->getNormalPlayTime(presentationTime);
				_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(_npt)).count();

				// notify timestamp log
				auto _msg = make_log_message("session(%s) timestamp : %llu", _path.c_str(), _timestamp);
				_listener->onReceivedLogFromLive555(live555_client_log_level::Debug, _msg);
			}

			// notify frame data
			_listener->onReceivedFrameDataFromLive555(&_sink->mUptrReceivedBuffer[0], frameSize, _width, _height, _timestamp, _codec, _path);
		}

		// Then continue, to request the next frame of data:
		_sink->continuePlaying();
	}
}


// StreamClientState
Live555ClientSource::RTSPClientImpl::StreamClientState::StreamClientState()
 : iter(nullptr)
 , session(nullptr)
 , subsession(nullptr)
 , streamTimerTask(nullptr)
 , duration(0.0)
{

}

Live555ClientSource::RTSPClientImpl::StreamClientState::~StreamClientState()
{
	if (nullptr != iter) {
		delete iter;
		iter = nullptr;
	}

	if (nullptr != session) {
		// We also need to delete "session", and unschedule "streamTimerTask" (if set)
		UsageEnvironment& env = session->envir(); // alias
		env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
		Medium::close(session);
	}
}


// RTSPClientImpl
Live555ClientSource::RTSPClientImpl::RTSPClientImpl(UsageEnvironment& env, char const* rtspURL, Live555ClientSource* source)
 : RTSPClient(env, rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, nullptr, 0, -1)
 , mClientSource(source)
{

}


// Live555ClientSource
Live555ClientSource::Live555ClientSource(Listener* listener)
 : mUptrLive555Scheduler(nullptr)
 , mUptrLive555Environment(nullptr, [] (UsageEnvironment* env) { if (env) { env->reclaim(); env = nullptr; }})
 , mUptrLive555Client(nullptr, [] (RTSPClientImpl* client) { if (client) { shutdownStream(client); client = nullptr; }})
 , mLive555EventLoopWatchVariable(1)
 , mListener(listener)
{

}

Live555ClientSource::~Live555ClientSource()
{
	close();
}

bool Live555ClientSource::open()
{
	if (!mUptrLive555Scheduler) {
#if defined(HAVE_EPOLL_SCHEDULER)
		mUptrLive555Scheduler.reset(EpollTaskScheduler::createNew());
#else //!HAVE_EPOLL_SCHEDULER
		mUptrLive555Scheduler.reset(BasicTaskScheduler::createNew());
#endif //HAVE_EPOLL_SCHEDULER
		assert(mUptrLive555Scheduler);

		mUptrLive555Environment.reset(BasicUsageEnvironment::createNew(*mUptrLive555Scheduler));
		assert(mUptrLive555Environment);

		if (mUptrLive555Scheduler && mUptrLive555Environment) {
			return true;
		}

		mUptrLive555Scheduler.reset();
		mUptrLive555Environment.reset();
	}

	return false;
}

bool Live555ClientSource::run(const std::string& url)
{
	if (mUptrLive555Environment) {
		mUptrLive555Client.reset(new RTSPClientImpl(*mUptrLive555Environment, url.c_str(), this));
		assert(mUptrLive555Client);

		if (mUptrLive555Client) {
			// Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
			// Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
			// Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
			mUptrLive555Client->sendDescribeCommand(continueAfterDESCRIBE);
			mStopScheduler = std::async([] (Live555ClientSource* thiz) {
				thiz->mLive555EventLoopWatchVariable = 0;
				thiz->mUptrLive555Environment->taskScheduler().doEventLoop(&thiz->mLive555EventLoopWatchVariable);
					
				if (nullptr != thiz->mListener) {
					thiz->mListener->onReceivedLogFromLive555(live555_client_log_level::Verbose, "taskScheduler done!");
				}
			}, this);

			if (mStopScheduler.valid()) {
				return true;
			}

			if (nullptr != mListener) {
				mListener->onReceivedLogFromLive555(live555_client_log_level::Warn, "mStopScheduler is not valid!");
			}
		}
	}

	return false;
}

void Live555ClientSource::stop()
{
	mLive555EventLoopWatchVariable = 1;
	if (mStopScheduler.valid()) {
		static constexpr int _timeout = 1000;	// 1000ms
		while (std::future_status::timeout == mStopScheduler.wait_for(std::chrono::milliseconds(_timeout))) {
			if (nullptr != mListener) {
				auto _msg = make_log_message("mStopScheduler timeout : %dms", _timeout);
				mListener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}
		}
	}

	mUptrLive555Client.reset();
}

void Live555ClientSource::close()
{
	stop();
	mUptrLive555Scheduler.reset();
}

live555_client_source_listener* Live555ClientSource::listener(RTSPClient* client)
{
	auto _impl = static_cast<RTSPClientImpl*>(client);
	if (nullptr != _impl && nullptr != _impl->mClientSource) {
		return _impl->mClientSource->mListener;
	}

	return nullptr;
}


// Implementation of the RTSP 'response handlers':
void Live555ClientSource::continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString)
{
	if (nullptr != rtspClient) {
		auto _listener = Live555ClientSource::listener(rtspClient);

		do {
			UsageEnvironment& env = rtspClient->envir(); // alias
			RTSPClientImpl::StreamClientState& scs = static_cast<RTSPClientImpl*>(rtspClient)->mStreamClientState; // alias

			if (resultCode != 0) {
				if (nullptr != _listener) {
					auto _msg = make_log_message("[\"%s\"]:Failed to get a SDP description: %s", rtspClient->url(), resultString);
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				delete[] resultString;
				break;
			}

			char* const sdpDescription = resultString;
			if (nullptr != _listener) {
				auto _msg = make_log_message("[\"%s\"]:Got a SDP description:\n%s", rtspClient->url(), sdpDescription);
				_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}

			// Create a media session object from this SDP description:
			scs.session = MediaSession::createNew(env, sdpDescription);
			delete[] sdpDescription; // because we don't need it anymore
			if (scs.session == NULL) {
				if (nullptr != _listener) {
					auto _msg = make_log_message("[\"%s\"]:Failed to create a MediaSession object from the SDP description: %s", rtspClient->url(), env.getResultMsg());
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				break;
			}
			else if (!scs.session->hasSubsessions()) {
				if (nullptr != _listener) {
					auto _msg = make_log_message("[\"%s\"]:This session has no media subsessions (i.e., no \"m=\" lines)", rtspClient->url());
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				break;
			}

			// Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
			// calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
			// (Each 'subsession' will have its own data source.)
			scs.iter = new MediaSubsessionIterator(*scs.session);
			setupNextSubsession(rtspClient);
			return;
		} while (0);

		// An unrecoverable error occurred with this stream.
		shutdownStream(rtspClient);
	}
}

void Live555ClientSource::continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString)
{
	if (nullptr != rtspClient) {
		auto _listener = Live555ClientSource::listener(rtspClient);

		do {
			UsageEnvironment& env = rtspClient->envir(); // alias
			RTSPClientImpl::StreamClientState& scs = static_cast<RTSPClientImpl*>(rtspClient)->mStreamClientState; // alias

			if (resultCode != 0) {
				if (nullptr != _listener) {
					auto _msg = make_log_message("[\"%s\"]:Failed to set up the \"%s/%s\" subsession: %s",
							rtspClient->url(), scs.subsession->mediumName(), scs.subsession->codecName(), resultString);
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				break;
			}

			std::string _msg("Set up the \"");
			_msg += scs.subsession->mediumName();
			_msg += "/";
			_msg += scs.subsession->codecName();
			_msg += "\" subsession (";
			if (scs.subsession->rtcpIsMuxed()) {
				_msg += "client port ";
				_msg += std::to_string(scs.subsession->clientPortNum());
			}
			else {
				_msg += "client port ";
				_msg += std::to_string(scs.subsession->clientPortNum());
				_msg += "-";
				_msg += std::to_string(scs.subsession->clientPortNum() + 1);
			}
			_msg += ")";
			if (nullptr != _listener) {
				_msg = make_log_message("[\"%s\"]:%s", rtspClient->url(), _msg.c_str());
				_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}

			// Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
			// (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
			// after we've sent a RTSP "PLAY" command.)

			scs.subsession->sink = new RTSPClientSink(env, *scs.subsession, static_cast<RTSPClientImpl*>(rtspClient)->mClientSource);
			// perhaps use your own custom "MediaSink" subclass instead
			if (scs.subsession->sink == NULL) {
				if (nullptr != _listener) {
					auto _msg = make_log_message("[\"%s\"]:Failed to create a data sink for the \"%s/%s\" subsession:%s",
						rtspClient->url(), scs.subsession->mediumName(), scs.subsession->codecName(), env.getResultMsg());
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				break;
			}

			if (nullptr != _listener) {
				auto _msg = make_log_message("[\"%s\"]:Created a data sink for the \"%s/%s\" subsession", 
						rtspClient->url(), scs.subsession->mediumName(), scs.subsession->codecName());
				_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}
			scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession
			scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
				subsessionAfterPlaying, scs.subsession);
			// Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
			if (scs.subsession->rtcpInstance() != NULL) {
				scs.subsession->rtcpInstance()->setByeWithReasonHandler(subsessionByeHandler, scs.subsession);
			}
		} while (0);
		delete[] resultString;

		// Set up the next subsession, if any:
		setupNextSubsession(rtspClient);
	}
}

void Live555ClientSource::continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString)
{
	if (nullptr != rtspClient) {
		Boolean success = False;
		auto _listener = Live555ClientSource::listener(rtspClient);

		do {
			UsageEnvironment& env = rtspClient->envir(); // alias
			RTSPClientImpl::StreamClientState& scs = static_cast<RTSPClientImpl*>(rtspClient)->mStreamClientState; // alias

			if (resultCode != 0) {
				if (nullptr != _listener) {
					auto _msg = make_log_message("[\"%s\"]:Failed to start playing session: %s", rtspClient->url(), resultString);
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				break;
			}

			// Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
			// using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
			// 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
			// (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
			if (scs.duration > 0) {
				unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
				scs.duration += delaySlop;
				unsigned uSecsToDelay = (unsigned)(scs.duration * 1000000);
				scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
			}

			std::string _msg("Started playing session");
			if (scs.duration > 0) {
				_msg += " (for up to ";
				_msg += std::to_string(scs.duration);
				_msg += " seconds)";
			}
			_msg += "...";

			if (nullptr != _listener) {
				_msg = make_log_message("[\"%s\"]:%s", rtspClient->url(), _msg.c_str());
				_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}
			success = True;
		} while (0);
		delete[] resultString;

		if (!success) {
			// An unrecoverable error occurred with this stream.
			shutdownStream(rtspClient);
		}
	}
}

// Implementation of the other event handlers:
void Live555ClientSource::subsessionAfterPlaying(void* clientData)
{
	MediaSubsession* subsession = static_cast<MediaSubsession*>(clientData);
	if (nullptr != subsession) {
		auto rtspClient = static_cast<RTSPClientImpl*>(subsession->miscPtr);
		if (nullptr != rtspClient) {
			if (nullptr != rtspClient && nullptr != rtspClient->mClientSource) {
				std::lock_guard<std::mutex> _lock(rtspClient->mClientSource->mSinkLock);

				// Begin by closing this subsession's stream:
				Medium::close(subsession->sink);
				subsession->sink = NULL;

				// Next, check whether *all* subsessions' streams have now been closed:
				MediaSession& session = subsession->parentSession();
				MediaSubsessionIterator iter(session);
				while ((subsession = iter.next()) != NULL) {
					if (subsession->sink != NULL) return; // this subsession is still active
				}
			}

			// All subsessions' streams have now been closed, so shutdown the client:
			shutdownStream(rtspClient);
		}
	}
}

void Live555ClientSource::subsessionByeHandler(void* clientData, char const* reason)
{
	MediaSubsession* subsession = static_cast<MediaSubsession*>(clientData);
	if (nullptr != subsession) {
		RTSPClient* rtspClient = static_cast<RTSPClient*>(subsession->miscPtr);
		if (nullptr != rtspClient) {
			UsageEnvironment& env = rtspClient->envir(); // alias

			std::string _msg("Received RTCP \"BYE\"");
			if (reason != NULL) {
				_msg += " (reason:\"";
				_msg += reason;
				_msg += "\")";
				delete[](char*)reason;
			}
			_msg += " on \"";
		
			auto _listener = Live555ClientSource::listener(rtspClient);
			if (nullptr != _listener) {
				_msg = make_log_message("[\"%s\"]:%s %s/%s \" subsession", rtspClient->url(), _msg.c_str(), subsession->mediumName(), subsession->codecName());
				_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}
		}

		// Now act as if the subsession had closed:
		subsessionAfterPlaying(subsession);
	}
}

void Live555ClientSource::streamTimerHandler(void* clientData)
{
	RTSPClientImpl* rtspClient = static_cast<RTSPClientImpl*>(clientData);
	if (nullptr != rtspClient) {
		RTSPClientImpl::StreamClientState& scs = rtspClient->mStreamClientState; // alias

		scs.streamTimerTask = NULL;

		// Shut down the stream:
		shutdownStream(rtspClient);
	}
}

void Live555ClientSource::setupNextSubsession(RTSPClient* rtspClient)
{
	if (nullptr != rtspClient) {
		UsageEnvironment& env = rtspClient->envir(); // alias
		RTSPClientImpl::StreamClientState& scs = static_cast<RTSPClientImpl*>(rtspClient)->mStreamClientState; // alias
		auto _listener = Live555ClientSource::listener(rtspClient);

		scs.subsession = scs.iter->next();
		if (scs.subsession != NULL) {
			if (!scs.subsession->initiate()) {
				setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
			}
			else {
				if (nullptr != _listener) {
					std::string _msg("");
					if (scs.subsession->rtcpIsMuxed()) {
						_msg = make_log_message("[\"%s\"]:Initiated the %s/%s subsession (client ports %d)",
								rtspClient->url(), scs.subsession->mediumName(), scs.subsession->codecName(), scs.subsession->clientPortNum());
					}
					else {
						_msg = make_log_message("[\"%s\"]:Initiated the %s/%s subsession (client ports %d-%d)",
								rtspClient->url(), scs.subsession->mediumName(), scs.subsession->codecName(),
								scs.subsession->clientPortNum(), scs.subsession->clientPortNum() + 1);
					}
						
					_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
				}
				// Continue setting up this subsession, by sending a RTSP "SETUP" command:
				rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
			}
			return;
		}

		// We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
		if (scs.session->absStartTime() != NULL) {
			// Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
			rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
		}
		else {
			scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
			rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
		}
	}
}

void Live555ClientSource::shutdownStream(RTSPClient* rtspClient)
{
	if (nullptr != rtspClient) {
		auto _source = static_cast<RTSPClientImpl*>(rtspClient)->mClientSource;
		auto _listener = Live555ClientSource::listener(rtspClient);

		if (nullptr != _source) {
			std::lock_guard<std::mutex> _lock(_source->mSinkLock);

			UsageEnvironment& env = rtspClient->envir(); // alias
			RTSPClientImpl::StreamClientState& scs = static_cast<RTSPClientImpl*>(rtspClient)->mStreamClientState; // alias

			// First, check whether any subsessions have still to be closed:
			if (scs.session != NULL) {
				Boolean someSubsessionsWereActive = False;
				MediaSubsessionIterator iter(*scs.session);
				MediaSubsession* subsession;

				while ((subsession = iter.next()) != NULL) {
					if (subsession->sink != NULL) {
						Medium::close(subsession->sink);
						subsession->sink = NULL;

						if (subsession->rtcpInstance() != NULL) {
							subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
						}

						someSubsessionsWereActive = True;
					}
				}

				if (someSubsessionsWereActive) {
					// Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
					// Don't bother handling the response to the "TEARDOWN".
					rtspClient->sendTeardownCommand(*scs.session, NULL);
				}
			}

			if (nullptr != rtspClient->url() && nullptr != _listener) {
				auto _msg = make_log_message("[\"%s\"]:Closing the stream.", rtspClient->url());
				_listener->onReceivedLogFromLive555(live555_client_log_level::Verbose, _msg);
			}

			Medium::close(rtspClient);
			// Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.
		}
	}
}

sptr_live555_client_source create_live555_client_source(live555_client_source_listener* listener)
{
	if (nullptr != listener) {
		return std::make_shared<Live555ClientSource>(listener);
	}

	return nullptr;
}
