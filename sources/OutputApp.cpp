/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>
 */

#if defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__) && !defined(WIN32)
	#define WIN32
#endif
#include <srt/srt.h>
#undef LOG_INFO
#undef LOG_DEBUG
#undef min
#undef max

#include "OutputApp.h"
#include "Mona/String.h"
#include "Mona/AVC.h"
#include "Mona/SocketAddress.h"
#include "Mona/Thread.h"

using namespace Mona;
using namespace std;

static const int64_t epollWaitTimoutMS = 250;

class OutputApp::Client::OpenSrtPIMPL : private Thread {
	private:
		::SRTSOCKET _socket;
		bool _started;
		SocketAddress _addr;
		std::mutex _mutex;
	public:
		OpenSrtPIMPL() :
			_socket(::SRT_INVALID_SOCK), _started(false), Thread("OutputApp") {
		}
		~OpenSrtPIMPL() {
			Close();
		}

		bool Open(const string& host) {
			if (_started) {
				ERROR("SRT Open: Already open, please close first")
				return false;
			}

			if (::srt_startup()) {
				ERROR("SRT Open: Error starting SRT library")
				return false;
			}
			_started = true;

			::srt_setloghandler(nullptr, logCallback);
			::srt_setloglevel(0xff);

			Exception ex;
			if (!_addr.set(ex, host) || _addr.family() != IPAddress::IPv4) {
				ERROR("SRT Open: can't resolve target host, ", host)
				Close();
				return false;
			}

			if (!Thread::start(ex)) {
				ERROR("SRT Open: can't start monitor thread")
				Close();
				return false;				
			}

			INFO("SRT opened")

			return true;
		}

		void Close() {
			Disconnect();
			Thread::stop();

			if (_started) {
				::srt_setloghandler(nullptr, nullptr);
				::srt_cleanup();
				_started = false;
			}

			_addr.reset();
		}

		bool Connect() {
			std::lock_guard<std::mutex> lock(_mutex);
			return ConnectActual();
		}

		bool ConnectActual() {
			if (!_addr) {
				ERROR("Open first")
				return false;
			}
			
			if (_socket != ::SRT_INVALID_SOCK) {
				ERROR("Already connected, please disconnect first")
				return false;
			}

			_socket = ::srt_socket(AF_INET, SOCK_DGRAM, 0);
			if (_socket == ::SRT_INVALID_SOCK ) {
				ERROR("SRT create socket: ", ::srt_getlasterror_str());
				return false;
			}

			bool block = false;
			int rc = ::srt_setsockopt(_socket, 0, SRTO_SNDSYN, &block, sizeof(block));
			if (rc != 0)
			{
				DisconnectActual();
				ERROR("SRT SRTO_SNDSYN: ", ::srt_getlasterror_str());
				return false;
			}
			rc = ::srt_setsockopt(_socket, 0, SRTO_RCVSYN, &block, sizeof(block));
			if (rc != 0)
			{
				ERROR("SRT SRTO_RCVSYN: ", ::srt_getlasterror_str());
				DisconnectActual();
				return false;
			}

			int opt = 1;
			::srt_setsockflag(_socket, ::SRTO_SENDER, &opt, sizeof opt);

			::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			if (state != SRTS_INIT) {
				ERROR("SRT Connect: socket is in bad state; ", state)
				DisconnectActual();
				return false;
			}

			INFO("Connecting to ", _addr.host(), " port ", _addr.port())

			// SRT support only IPV4 so we convert to a sockaddr_in
			sockaddr addr;
			memcpy(&addr, _addr.data(), sizeof(sockaddr)); // WARN: work only with ipv4 addresses
			addr.sa_family = AF_INET;
			if (::srt_connect(_socket, &addr, sizeof(sockaddr))) {
				ERROR("SRT Connect: ", ::srt_getlasterror_str());
				DisconnectActual();
				return false;
			}

			INFO("SRT connect state; ", ::srt_getsockstate(_socket));

			return true;
		}

		bool Disconnect() {
			std::lock_guard<std::mutex> lock(_mutex);
			return DisconnectActual();
		}

		bool DisconnectActual() {
			if (_socket != ::SRT_INVALID_SOCK) {
				::srt_close(_socket);

				INFO("SRT disconnect state; ", ::srt_getsockstate(_socket));

				_socket = ::SRT_INVALID_SOCK;
			}

			return true;
		}

		int Write(shared_ptr<Buffer>& pBuffer)
		{
			std::lock_guard<std::mutex> lock(_mutex);

			UInt8* p = pBuffer->data();
			const size_t psize = pBuffer->size();

			if (_socket == ::SRT_INVALID_SOCK) {
				WARN("SRT: Drop packet while NOT CONNECTED")
				return psize;
			}

			SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			switch(state) {
				case ::SRTS_CONNECTED: {
					// No-op
				}
				break;
				default: {
					if ((false)) {
						DEBUG("SRT: Drop packet on state ", state)
					}
					return psize;
				}
				break;
			}

			for (size_t i = 0; i < psize;) {
				size_t chunk = min<size_t>(psize - i, (size_t)1316);
				if (::srt_sendmsg(_socket,
						(const char*)(p + i), chunk, -1, true) < 0)
					WARN("SRT: send error; ", ::srt_getlasterror_str())
				i += chunk;
			}

			return psize;
		}

		bool run(Exception&, const volatile bool& requestStop) {

			_mutex.lock();

			int epollid = ::srt_epoll_create();
			if (epollid < 0) {
				ERROR("Error initializing UDT epoll set;",
					::srt_getlasterror_str());
			}

			while(!requestStop && epollid >= 0) {
				::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
				if (state == ::SRTS_BROKEN || state == ::SRTS_NONEXIST
						|| state == ::SRTS_CLOSED) {
					INFO("Reconnect socket");
					if (_socket != ::SRT_INVALID_SOCK) {
						DEBUG("Remove socket from poll; ", (int)_socket);
						::srt_epoll_remove_usock(epollid, _socket);
					}

					DisconnectActual();
					if (!ConnectActual()) {

						ERROR("Error issuing connect");
						break;
					}

					FATAL_CHECK(_socket != ::SRT_INVALID_SOCK)
					int modes = SRT_EPOLL_IN;
					if (::srt_epoll_add_usock(epollid, _socket, &modes) != 0) {
						ERROR("Error adding socket to poll set; ",
							::srt_getlasterror_str());
						break;
					}
				}

				_mutex.unlock();
				const int socksToPoll = 10;
				int rfdn = socksToPoll;
				::SRTSOCKET rfds[socksToPoll];
				int rc = ::srt_epoll_wait(epollid, &rfds[0], &rfdn, nullptr, nullptr,
					epollWaitTimoutMS, nullptr, nullptr, nullptr, nullptr);

				if (rc <= 0) {
					// Let the system breath just in case
					Sleep(0);

					_mutex.lock();
					continue;
				}
				_mutex.lock();

				FATAL_CHECK(rfdn <= socksToPoll)

				for (int i = 0; i < rfdn; i++) {
					::SRTSOCKET socket = rfds[i];
					state = ::srt_getsockstate(socket);
					switch(state) {
						case ::SRTS_CONNECTED: {
							// Discard incoming data
							static char buf[1500];
							while (::srt_recvmsg(socket, &buf[0], sizeof(buf)) > 0)
								continue;
						}
						break;
						case ::SRTS_NONEXIST:
						case ::SRTS_BROKEN:
						case ::SRTS_CLOSING:
						case ::SRTS_CLOSED: {
							DEBUG("Remove socket from poll (on poll event); ", socket);
							::srt_epoll_remove_usock(epollid, socket);
						}
						break;
						default: {
							WARN("Unexpected event on ",  socket, "state ", state);
						}
						break;
					}
				}
			}

			// TODO: debug this
			if ((false) && epollid > 0)
				::srt_epoll_release(epollid);

			_mutex.unlock();
			return true;
		}

private:

		static void logCallback(void* opaque, int level, const char* file,
				int line, const char* area, const char* message)
		{
			INFO("L:", level, "|", file, "|", line, "|", area, "|", message)
		}
};

OutputApp::OutputApp(const Parameters& configs): App(configs)
{
	_target.assign(configs.getString("srt.target", "localhost:4900"));
}

OutputApp::~OutputApp() {
}

OutputApp::Client::Client(Mona::Client& client, const string& host) : App::Client(client), _first(true), _pPublication(NULL), _videoCodecSent(false), _audioCodecSent(false),
	_srtPimpl(new OutputApp::Client::OpenSrtPIMPL()) {

	FATAL_CHECK(_srtPimpl.get() != nullptr);
	_srtPimpl->Open(host);

	_onAudio = [this](UInt16 track, const Media::Audio::Tag& tag, const Packet& packet) {
		/*if (_stopping)
			return resetSRT();
		*/
		shared<Buffer> pBuffer;

		// AAC codecs to be sent in first
		if (!_audioCodecSent) {
			if (tag.codec == Media::Audio::CODEC_AAC && tag.isConfig) {

				INFO("AAC codec infos saved")
				_audioCodec.set(std::move(packet));
			}
			if (!_audioCodec)
				return;

			_audioCodecSent = true;
			INFO("AAC codec infos sent")
			Media::Audio::Tag configTag(tag);
			configTag.isConfig = true;
			configTag.time = tag.time;
			if (!tag.isConfig && !writePayload(0, writeFrame(pBuffer, configTag, _audioCodec)))
				return;
		}

		if (!writePayload(0, writeFrame(pBuffer, tag, packet))) 
			return;
	};
	_onVideo = [this](UInt16 track, const Media::Video::Tag& tag, const Packet& packet) {
		/*if (_stopping)
			return resetSRT();*/
		shared<Buffer> pBuffer;

		// Video codecs to be sent in first
		if (!_videoCodecSent) {

			Packet sps, pps;
			bool isAVCConfig(tag.codec == Media::Video::CODEC_H264 && tag.frame == Media::Video::FRAME_CONFIG && AVC::ParseVideoConfig(packet, sps, pps));
			if (isAVCConfig) {
				INFO("Video codec infos saved")
				_videoCodec.set(std::move(packet));
			}
			if (!_videoCodec)
				return;

			if (tag.frame != Media::Video::FRAME_KEY) {
				DEBUG("Video frame dropped to wait first key frame")
				return;
			}

			_videoCodecSent = true;
			INFO("Video codec infos sent")
			Media::Video::Tag configTag(tag);
			configTag.frame = Media::Video::FRAME_CONFIG;
			configTag.time = tag.time;
			if (!isAVCConfig && !writePayload(0, writeFrame(pBuffer, configTag, _videoCodec)))
				return;
		}
		// Send Regularly the codec infos (TODO: Add at timer?)
		else if (tag.codec == Media::Video::CODEC_H264 && tag.frame == Media::Video::FRAME_KEY) {
			INFO("Sending codec infos")
			Media::Video::Tag configTag(tag);
			configTag.frame = Media::Video::FRAME_CONFIG;
			configTag.time = tag.time;
			if (!writePayload(0, writeFrame(pBuffer, configTag, _videoCodec)))
				return;
		}

		if (!writePayload(0, writeFrame(pBuffer, tag, packet)))
			return;
	};
	_onEnd = [this]() {
		resetSRT();
	};
	INFO("A new publish client is connecting from ", client.address);
}

OutputApp::Client::~Client() {
	INFO("Client from ", client.address, " is disconnecting...")

	_srtPimpl->Close();

	resetSRT();
}

bool OutputApp::Client::onPublish(Exception& ex, Publication& publication) {
	INFO("Client from ", client.address, " is trying to publish ", publication.name())

	if (_pPublication) {
		WARN("Client is already publishing, request ignored")
		return false;
	}

	// Init parameters
	publication.onAudio = _onAudio;
	publication.onVideo = _onVideo;
	publication.onEnd = _onEnd;
	_pPublication = &publication;

	return true;
}

void OutputApp::Client::onUnpublish(Publication& publication) {
	INFO("Client from ", client.address, " has closed publication ", publication.name(), ", stopping the injection...")

	resetSRT();
}

void OutputApp::Client::resetSRT() {

	if (_pPublication) {
		_pPublication->onAudio = nullptr;
		_pPublication->onVideo = nullptr;
		_pPublication->onEnd = nullptr;
		_pPublication = NULL;
	}

	_tsWriter.endMedia([](const Packet& packet) {}); // reset the ts writer
	_videoCodec.reset();
	_audioCodec.reset();
	_first = false;
}

template <>
void OutputApp::Client::writeMedia<Media::Video::Tag>(Mona::BinaryWriter& writer, const Media::Video::Tag& tag, const Mona::Packet& packet) {
	_tsWriter.writeVideo(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
}

template <>
void OutputApp::Client::writeMedia<Media::Audio::Tag>(Mona::BinaryWriter& writer, const Media::Audio::Tag& tag, const Mona::Packet& packet) {
	_tsWriter.writeAudio(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
}

bool OutputApp::Client::writePayload(UInt16 context, shared_ptr<Buffer>& pBuffer) {

	int res = _srtPimpl->Write(pBuffer);

	return (res == (int)pBuffer->size());
}

OutputApp::Client* OutputApp::newClient(Mona::Exception& ex, Mona::Client& client, Mona::DataReader& parameters, Mona::DataWriter& response) {

	return new Client(client, _target);
}
