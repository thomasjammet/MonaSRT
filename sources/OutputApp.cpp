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


#include <srt/srt.h>
#undef LOG_INFO
#undef LOG_DEBUG
#undef min
#undef max

#include "OutputApp.h"
#include "Mona/String.h"
#include "Mona/AVC.h"
#include "Mona/SocketAddress.h"

using namespace Mona;
using namespace std;

class OutputApp::Client::OpenSrtPIMPL {
	private:
		::SRTSOCKET _socket;
		bool _started;
		SocketAddress _addr;
	public:
		OpenSrtPIMPL() :
			_socket(::SRT_INVALID_SOCK), _started(false) {
		}
		~OpenSrtPIMPL() {
			Close();
		}

		bool Open(const string& host) {
			if (_started) {
				ERROR("Already open, please close first")
				return false;
			}

			if (::srt_startup()) {
				ERROR("Error starting SRT library")
				return false;
			}
			_started = true;

			::srt_setloghandler(nullptr, logCallback);
			::srt_setloglevel(0xff);

			Exception ex;
			if (!_addr.set(ex, host) || _addr.family() != IPAddress::IPv4) {
				ERROR("SRT Connect: can't resolve target host, ", host)
				Close();
				return false;
			}

			INFO("SRT opened")

			return true;
		}

		void Close() {
			Disconnect();

			if (_started) {
				::srt_setloghandler(nullptr, nullptr);
				::srt_cleanup();
				_started = false;
			}

			_addr.reset();
		}

		bool Connect() {
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
				ERROR("SRT Connect: ", ::srt_getlasterror_str());
				return false;
			}

			int opt = 1;
			::srt_setsockflag(_socket, ::SRTO_SENDER, &opt, sizeof opt);

			::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			if (state != SRTS_INIT) {
				ERROR("SRT Connect: socket is in bad state; ", state)
				Disconnect();
				return false;
			}

			INFO("Connecting to ", _addr.host(), " port ", _addr.port())

			if ((true)) {
				// Mona BUG: _addr.size() always returns sizeof(sockaddr_in6)
				FATAL_CHECK(_addr.size() >= sizeof(sockaddr));

				// Mona BUG: family is always set to AF_INET6
				sockaddr addr;
				memcpy(&addr, _addr.data(), sizeof(sockaddr));
				addr.sa_family = AF_INET;
				if (::srt_connect(_socket, &addr, sizeof(sockaddr))) {
					ERROR("SRT Connect: ", ::srt_getlasterror_str());
					Disconnect();
					return false;
				}
			} else {
				if (::srt_connect(_socket, _addr.data(), _addr.family() == IPAddress::IPv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in))) {
					ERROR("SRT Connect: ", ::srt_getlasterror_str());
					Disconnect();
					return false;
				}
			}

			INFO("SRT connect state; ", ::srt_getsockstate(_socket));

			return true;
		}

		bool Disconnect() {
			if (_socket != ::SRT_INVALID_SOCK) {
				::srt_close(_socket);

				INFO("SRT disconnect state; ", ::srt_getsockstate(_socket));

				_socket = ::SRT_INVALID_SOCK;
			}

			return true;
		}

		int Write(shared_ptr<Buffer>& pBuffer)
		{
			if (_socket == ::SRT_INVALID_SOCK && !Connect()) {
				ERROR("Failed to re-connect");
				return -1;
			}

			SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			switch(state) {
				case ::SRTS_CONNECTING:
					WARN("SRT: Drop packet while CONNECTING")
					return pBuffer->size();
				case ::SRTS_CONNECTED:
					break;
				default:
					WARN("SRT: Try to re-connect on bad state; ", state)
					Disconnect();
					if (!Connect()) {
						ERROR("SRT: Failed to re-connect; ", ::srt_getlasterror_str())
						return -1;
					}
			}

			UInt8* p = pBuffer->data();
			const size_t psize = pBuffer->size();
			for (size_t i = 0; i < psize;) {
				size_t chunk = min<size_t>(psize - i, (size_t)1316);
				if (::srt_sendmsg(_socket,
						(const char*)(p + i), chunk, -1, true) < 0)
					WARN("SRT: send error; ", ::srt_getlasterror_str())
				i += chunk;
			}

			return psize;
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

		// AAC codecs to be sent in first
		if (!_audioCodecSent) {
			if (tag.codec == Media::Audio::CODEC_AAC && tag.isConfig) {

				INFO("AAC codec infos saved")
				_audioCodec.set(std::move(packet));
				_audioTag.reset(new Media::Audio::Tag(tag));
			}
			if (!_audioCodec)
				return;

			_audioCodecSent = true;
			INFO("AAC codec infos sent")
			if (!tag.isConfig) {
				shared<Buffer> pBuffer(new Buffer());
				BinaryWriter writer(*pBuffer);
				if (_first) {
					_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
					_first = false;
				}
				_tsWriter.writeAudio(0, *_audioTag, _audioCodec, [&writer](const Packet& output) { writer.write(output); });
				if (!writePayload(0, pBuffer)) {
					//_stopping = true;
					return;
				}
			}
		}

		shared<Buffer> pBuffer(new Buffer());
		BinaryWriter writer(*pBuffer);
		if (_first) {
			_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
			_first = false;
		}
		_tsWriter.writeAudio(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
		if (!writePayload(0, pBuffer)) {
			//_stopping = true;
			return;
		}
	};
	_onVideo = [this](UInt16 track, const Media::Video::Tag& tag, const Packet& packet) {
		/*if (_stopping)
			return resetSRT();*/

		// Video codecs to be sent in first
		if (!_videoCodecSent) {

			Packet sps, pps;
			bool isAVCConfig(tag.codec == Media::Video::CODEC_H264 && tag.frame == Media::Video::FRAME_CONFIG && AVC::ParseVideoConfig(packet, sps, pps));
			if (isAVCConfig) {
				INFO("Video codec infos saved")
				_videoCodec.set(std::move(packet));
				_videoTag.reset(new Media::Video::Tag(tag));
			}
			if (!_videoCodec)
				return;

			if (tag.frame != Media::Video::FRAME_KEY) {
				DEBUG("Video frame dropped to wait first key frame")
				return;
			}

			_videoCodecSent = true;
			INFO("Video codec infos sent")
			if (!isAVCConfig) {
				shared<Buffer> pBuffer(new Buffer());
				BinaryWriter writer(*pBuffer);
				if (_first) {
					_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
					_first = false;
				}
				_tsWriter.writeVideo(0, *_videoTag, _videoCodec, [&writer](const Packet& output) { writer.write(output); });
				if (!writePayload(0, pBuffer)) {
					//_stopping = true;
					return;
				}
			}
		}

		shared<Buffer> pBuffer(new Buffer());
		BinaryWriter writer(*pBuffer);
		if (_first) {
			_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
			_first = false;
		}
		_tsWriter.writeVideo(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
		if (!writePayload(0, pBuffer)) {
			//_stopping = true;
			return;
		}
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

	// Start SRT Publication
	_srtPimpl->Connect();

	return true;
}

void OutputApp::Client::onUnpublish(Publication& publication) {
	INFO("Client from ", client.address, " has closed publication ", publication.name(), ", stopping the injection...")

	_srtPimpl->Disconnect();

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
	_videoTag.reset();
	_audioCodec.reset();
	_audioTag.reset();
	_first = false;
}

bool OutputApp::Client::writePayload(UInt16 context, shared_ptr<Buffer>& pBuffer) {

	int res = _srtPimpl->Write(pBuffer);

	return (res == (int)pBuffer->size());
}

OutputApp::Client* OutputApp::newClient(Mona::Exception& ex, Mona::Client& client, Mona::DataReader& parameters, Mona::DataWriter& response) {

	return new Client(client, _target);
}
